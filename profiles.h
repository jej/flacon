/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * Flacon - audio File Encoder
 * https://github.com/flacon/flacon
 *
 * Copyright: 2019-2020
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

#ifndef PROFILES_H
#define PROFILES_H

#include <QString>
#include <QVariant>
#include <QVector>

#include "formats_out/outformat.h"
class QSettings;

class Profile
{
    friend QDebug operator<<(QDebug dbg, const Profile &profile);

public:
    Profile();
    explicit Profile(const QString &id);
    explicit Profile(OutFormat &format, const QString &id = "");
    Profile(const Profile &other) = default;
    Profile &operator=(const Profile &other) = default;

    QString id() const { return mId; }

    QString name() const { return mName; }
    void    setName(const QString &value);

    QVariant value(const QString &key, const QVariant &defaultValue = QVariant()) const;
    void     setValue(const QString &key, const QVariant &value);

    bool isValid() const noexcept;

    QString outFileDir() const;
    void    setOutFileDir(const QString &value);

    QString outFilePattern() const;
    void    setOutFilePattern(const QString &value);

    GainType gainType() const;
    void     setGainType(GainType value);

    int  bitsPerSample() const;
    void setBitsPerSample(int value);

    SampleRate sampleRate() const;
    void       setSampleRate(SampleRate value);

    bool isCreateCue() const;
    void setCreateCue(bool value);

    bool isEmbedCue() const;
    void setEmbedCue(bool value);

    QString cueFileName() const;
    void    setCueFileName(const QString &value);

    PreGapType preGapType() const;
    void       setPregapType(PreGapType value);

    const OutFormat *  outFormat() const { return mFormat; }
    QString            formatId() const { return mFormat->id(); }
    QString            formatName() const { return mFormat->name(); }
    QString            ext() const { return mFormat->ext(); }
    FormatOptions      formatOptions() const { return mFormat->options(); }
    BitsPerSample      maxBitPerSample() const { return mFormat->maxBitPerSample(); }
    SampleRate         maxSampleRate() const { return mFormat->maxSampleRate(); }
    EncoderConfigPage *configPage(QWidget *parent) const;

    // Cover options ............................
    CoverOptions copyCoverOptions() const;
    void         setCopyCoverOptions(const CoverOptions &copyCoverOptions);

    CoverOptions embedCoverOptions() const;
    void         setEmbedCoverOptions(const CoverOptions &embedCoverOptions);

    void load(QSettings &settings, const QString &group);
    void save(QSettings &settings, const QString &group) const;

private:
    QString                  mId;
    const OutFormat *        mFormat;
    QString                  mName;
    QHash<QString, QVariant> mValues;
    void                     setDefaultValues();

    CoverOptions mCopyCoverOptions;
    CoverOptions mEmbedCoverOptions;
    bool         mSupportEmbedCover = false;
};

Profile &NullProfile();

class Profiles : public QVector<Profile>
{
public:
    int  indexOf(const QString &id, int from = 0) const;
    bool update(const Profile &profile);
};

QDebug operator<<(QDebug dbg, const Profile &profile);
QDebug operator<<(QDebug dbg, const Profiles &profiles);

#endif // PROFILES_H
