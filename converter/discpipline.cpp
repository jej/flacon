/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * Flacon - audio File Encoder
 * https://github.com/flacon/flacon
 *
 * Copyright: 2017
 *   Alexander Sokoloff <sokoloff.a@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * END_COMMON_COPYRIGHT_HEADER */

#include "discpipline.h"

#include <QUuid>

#include "splitter.h"
#include "encoder.h"
#include "cuecreator.h"
#include "project.h"
#include "inputaudiofile.h"
#include "profiles.h"
#include "formats_out/metadatawriter.h"

#include <QThread>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <errno.h>
#include <QLoggingCategory>
#include <QBuffer>

namespace {
Q_LOGGING_CATEGORY(LOG, "DiscPipeline")
}

using namespace Conv;

/************************************************
 *
 ************************************************/
class Conv::WorkerThread : public QThread
{
public:
    explicit WorkerThread(Worker *worker, QObject *parent = nullptr);
    virtual ~WorkerThread() override;

    void run() override;

private:
    Worker *mWorker;
};

/************************************************
 *
 ************************************************/
WorkerThread::WorkerThread(Worker *worker, QObject *parent) :
    QThread(parent),
    mWorker(worker)
{
    worker->moveToThread(this);
}

/************************************************
 *
 ************************************************/
WorkerThread::~WorkerThread()
{
    quit();
    if (!wait(3000)) {
        qWarning() << "Can't quit from thread" << mWorker;
        this->terminate();
        if (!wait(3000))
            qWarning() << "Can't terminate from thread" << mWorker;
    }
    mWorker->deleteLater();
}

/************************************************
 *
 ************************************************/
void WorkerThread::run()
{
    mWorker->run();
}

/************************************************
 *
 ************************************************/
DiscPipeline::DiscPipeline(const Profile &profile, Disc *disc, ConvTracks tracks, const QString &workDir, QObject *parent) noexcept(false) :
    QObject(parent),
    mProfile(profile),
    mDisc(disc),
    mWorkDir(workDir)
{

    QString dir = QFileInfo(mWorkDir).dir().absolutePath();
    qCDebug(LOG) << "Create tmp dir" << dir;

    createDir(dir);

    mTmpDir = new QTemporaryDir(QString("%1/tmp").arg(dir));
    mTmpDir->setAutoRemove(true);

    for (const ConvTrack &track : qAsConst(tracks)) {
        if (track.audioFile().channelsCount() > 2) {
            mProfile.setGainType(GainType::Disable);
        }

        mTracks << track;
        mTrackStates[track.index()] = TrackState::NotRunning;

        qCDebug(LOG) << "Create directory for output files" << dir;
        createDir(QFileInfo(track.resultFilePath()).absoluteDir().path());
    }

    addSpliterRequest();
}

/************************************************

 ************************************************/
DiscPipeline::~DiscPipeline()
{
    delete mTmpDir;
}

/************************************************
 CREATE WORKER CHAINS
 ************************************************
              +--> Encoder ---> +
   Splitter ->+            ...  +-> writeGain --> trackDone
              +--> Encoder ---> +
 ************************************************/
void DiscPipeline::startWorker(int *splitterCount, int *count)
{
    if (mInterrupted) {
        return;
    }

    if (*count <= 0) {
        return;
    }

    if (*splitterCount > 0 && !mSplitterRequests.isEmpty()) {
        SplitterRequest req = mSplitterRequests.takeFirst();
        startSplitter(req);
        --(*splitterCount);
        --(*count);
        return;
    }

    while (*count > 0 && !mEncoderRequests.isEmpty()) {
        const Request req = mEncoderRequests.takeFirst();
        startEncoder(req.track, req.inputFile);
        --(*count);
    }
}

/************************************************
 *
 ************************************************/

void DiscPipeline::addSpliterRequest()
{
    QString outDir = mTmpDir->path();

    PreGapType pregapType = (hasPregap() && mProfile.isCreateCue()) ? mProfile.preGapType() : PreGapType::Skip;

    mSplitterRequests << SplitterRequest { mTracks, outDir, pregapType };
}

/************************************************
 *
 ************************************************/
void DiscPipeline::startSplitter(const SplitterRequest &request)
{
    Splitter *splitter = new Splitter(mDisc, request.tracks, request.outDir);
    splitter->setPregapType(request.pregapType);
    WorkerThread *thread = new WorkerThread(splitter, this);

    connect(this, &DiscPipeline::stopAllThreads, thread, &Conv::WorkerThread::deleteLater);
    connect(splitter, &Splitter::trackProgress, this, &DiscPipeline::trackProgress);
    connect(splitter, &Worker::error, this, &DiscPipeline::trackError);
    connect(splitter, &Splitter::trackReady, this, &DiscPipeline::addEncoderRequest);
    connect(thread, &Conv::WorkerThread::finished, this, &DiscPipeline::threadFinished);

    mThreads << thread;
    thread->start();

    for (const ConvTrack &t : request.tracks) {
        mTrackStates[t.index()] = TrackState::Splitting;
    }

    // *********************************************************
    // Short tasks, we do not allocate separate threads for them.
    try {
        copyCoverImage();
        createEmbedImage();
        writeOutCueFile();
        loadEmbeddedCue();
    }
    catch (const FlaconError &err) {
        trackError(request.tracks.first(), err.what());
    }
}

/************************************************
 *
 ************************************************/
void DiscPipeline::addEncoderRequest(const ConvTrack &track, const QString &inputFile)
{
    mEncoderRequests << Request { track, inputFile };
    trackProgress(track, TrackState::Queued, 0);
    emit readyStart();
}

/************************************************
 *
 ************************************************/
void DiscPipeline::startEncoder(const ConvTrack &track, const QString &inputFile)
{
    QFileInfo trackFile(track.resultFilePath());
    QString   outFile = QDir(mTmpDir->path()).filePath(QFileInfo(inputFile).baseName() + ".encoded." + trackFile.suffix());

    Encoder *encoder = mProfile.outFormat()->createEncoder();
    encoder->setInputFile(inputFile);
    encoder->setOutFile(outFile);
    encoder->setTrack(track);
    encoder->setProfile(mProfile);
    encoder->setEmbeddedCue(mEmbeddedCue);
    encoder->setCoverImage(mCoverImage);

    WorkerThread *thread = new WorkerThread(encoder, this);

    connect(this, &DiscPipeline::stopAllThreads, thread, &Conv::WorkerThread::deleteLater);
    connect(encoder, &Encoder::trackProgress, this, &DiscPipeline::trackProgress);
    connect(encoder, &Encoder::error, this, &DiscPipeline::trackError);

    // Replaygain ...............................
    if (mProfile.gainType() != GainType::Disable) {
        connect(encoder, &Encoder::trackReady, this, &DiscPipeline::writeGain);
    }
    else {
        connect(encoder, &Encoder::trackReady, this, &DiscPipeline::trackDone);
    }
    // ..........................................

    connect(thread, &Conv::WorkerThread::finished, this, &DiscPipeline::threadFinished);

    mThreads << thread;
    thread->start();
}

/************************************************
 *
 ************************************************/
void DiscPipeline::writeGain(const ConvTrack &track, const QString &fileName, const ReplayGain::Result &trackGain)
{
    qCDebug(LOG) << "Write track gain: " << fileName << "gain:" << trackGain.gain() << "peak:" << track;

    MetadataWriter *writer = mProfile.outFormat()->createMetadataWriter(fileName);
    writer->setTrackReplayGain(trackGain.gain(), trackGain.peak());
    writer->save();
    delete writer;

    if (mProfile.gainType() != GainType::Album) {
        trackDone(track, fileName);
        return;
    }

    mAlbumGain.add(trackGain);
    trackProgress(track, TrackState::WaitGain, 0);
    mAlbumGainRequests << Request { track, fileName };

    if (mAlbumGainRequests.count() < mTracks.count()) {
        return;
    }

    for (const Request &r : mAlbumGainRequests) {
        qCDebug(LOG) << "Write album gain: " << r.inputFile << "gain:" << mAlbumGain.result().gain() << "peak:" << mAlbumGain.result().peak();

        MetadataWriter *writer = mProfile.outFormat()->createMetadataWriter(r.inputFile);
        writer->setAlbumReplayGain(mAlbumGain.result().gain(), mAlbumGain.result().peak());
        writer->save();
        delete writer;

        trackDone(r.track, r.inputFile);
    }
}

/************************************************
 *
 ************************************************/
void DiscPipeline::trackDone(const ConvTrack &track, const QString &outFileName)
{
    qCDebug(LOG) << "Track done: "
                 << "index=" << track.index()
                 << track
                 << "outFileName:" << outFileName;

    // Track is ready, rename the file to the final name.
    // Remove old already existing file.
    QFile::remove(track.resultFilePath());

    QFile file(outFileName);
    if (!file.rename(track.resultFilePath())) {
        trackError(track, tr("I can't rename file:\n%1 to %2\n%3").arg(outFileName, track.resultFilePath(), file.errorString()));
    }

    mTrackStates[track.index()] = TrackState::OK;
    emit trackProgressChanged(track, TrackState::OK, 0);
    emit threadFinished();

    if (!isRunning()) {
        qCDebug(LOG) << "pipline finished";
        emit finished();
    }
}

/************************************************
 *
 ************************************************/
void DiscPipeline::createDir(const QString &dirName) const
{
    QDir dir(dirName);

    if (!dir.mkpath(".")) {
        QString msg = QObject::tr("I can't create directory \"%1\".").arg(dir.path());
        QString err = strerror(errno);
        throw FlaconError(msg + "<br><br>" + err);
    }

    if (!QFileInfo(dir.path()).isWritable()) {
        QString msg = QObject::tr("I can't write to directory \"%1\".").arg(dir.path());
        QString err = strerror(errno);
        throw FlaconError(msg + "<br><br>" + err);
    }
}

/************************************************

 ************************************************/
void DiscPipeline::interrupt(TrackState state)
{
    mInterrupted = true;
    mEncoderRequests.clear();

    for (ConvTrack &track : mTracks) {
        switch (mTrackStates[track.index()]) {
            case TrackState::Splitting:
            case TrackState::Encoding:
            case TrackState::Queued:
            case TrackState::WaitGain:
            case TrackState::CalcGain:
            case TrackState::WriteGain:
            case TrackState::NotRunning:
                mTrackStates[track.index()] = state;
                emit trackProgressChanged(track, state, 0);
                break;

            case TrackState::Canceled:
            case TrackState::Error:
            case TrackState::Aborted:
            case TrackState::OK:
                break;
        }
    }
}

/************************************************

 ************************************************/
void DiscPipeline::stop()
{
    interrupt(TrackState::Aborted);
    emit stopAllThreads();
    emit threadFinished();

    emit finished();
}

/************************************************

 ************************************************/
void DiscPipeline::trackError(const ConvTrack &track, const QString &message)
{
    mTrackStates[track.index()] = TrackState::Error;
    emit trackProgressChanged(track, TrackState::Error, 0);
    interrupt(TrackState::Aborted);
    emit stopAllThreads();
    emit threadFinished();

    emit finished();
    Messages::error(message);
}

/************************************************

 ************************************************/
bool DiscPipeline::isRunning() const
{
    for (TrackState state : mTrackStates) {
        switch (state) {
            case TrackState::Splitting:
            case TrackState::Encoding:
            case TrackState::Queued:
            case TrackState::WaitGain:
            case TrackState::CalcGain:
            case TrackState::WriteGain:
                return true;

            case TrackState::NotRunning:
            case TrackState::Canceled:
            case TrackState::Error:
            case TrackState::Aborted:
            case TrackState::OK:
                break;
        }
    }

    return false;
}

/************************************************
 *
 ************************************************/
int DiscPipeline::runningThreadCount() const
{
    int res = 0;
    foreach (WorkerThread *thread, mThreads) {
        if (thread->isRunning())
            ++res;
    }
    return res;
}

/************************************************

 ************************************************/
void DiscPipeline::trackProgress(const ConvTrack &track, TrackState state, int percent)
{
    if (mInterrupted)
        return;

    mTrackStates[track.index()] = state;
    emit trackProgressChanged(track, state, percent);
}

/************************************************
 *
 ************************************************/
void DiscPipeline::copyCoverImage() const
{
    QString file = mProfile.copyCoverOptions().mode != CoverMode::Disable ? mDisc->coverImageFile() : "";
    int     size = mProfile.copyCoverOptions().mode == CoverMode::Scale ? mProfile.copyCoverOptions().size : 0;

    if (file.isEmpty()) {
        return;
    }

    QString dir  = QFileInfo(mTracks.first().resultFilePath()).dir().absolutePath();
    QString dest = QDir(dir).absoluteFilePath(QString("cover.%1").arg(QFileInfo(file).suffix()));

    CoverImage image = CoverImage(file, size);
    image.saveAs(dest);
}

/************************************************
 *
 ************************************************/
void DiscPipeline::createEmbedImage()
{
    QString file = mProfile.embedCoverOptions().mode != CoverMode::Disable ? mDisc->coverImageFile() : "";
    int     size = mProfile.embedCoverOptions().mode == CoverMode::Scale ? mProfile.embedCoverOptions().size : 0;

    if (file.isEmpty()) {
        return;
    }

    mCoverImage = CoverImage(file, size);

    QString tmpCoverFile = QDir(mTmpDir->path()).absoluteFilePath(QString("cover.%1").arg(QFileInfo(file).suffix()));
    mCoverImage.saveTmpFile(tmpCoverFile);
}

/************************************************
 *
 ************************************************/
void DiscPipeline::writeOutCueFile()
{
    if (!mProfile.isCreateCue()) {
        return;
    }

    CueCreator cue(mDisc, mProfile.preGapType());
    cue.writeToFile(mProfile.cueFileName());
}

/************************************************
 *
 ************************************************/
void DiscPipeline::loadEmbeddedCue()
{
    if (!mProfile.isEmbedCue()) {
        return;
    }

    CueCreator cue(mDisc, mProfile.preGapType());
    QBuffer    buf;
    cue.write(&buf);
    mEmbeddedCue = QString::fromUtf8(buf.data());
}

/************************************************
 *
 ************************************************/
bool DiscPipeline::hasPregap() const
{
    return mTracks.first().index() == 0 &&                  // We extract first track in Audio
            mTracks.first().cueIndex(1).milliseconds() > 0; // The first track don't start from zero second
}
