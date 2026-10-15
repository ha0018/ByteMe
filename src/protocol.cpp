#include "protocol.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtEndian>

QByteArray Wire::pack(Kind kind, const QByteArray &payload)
{
    QByteArray result(4, '\0');
    qToBigEndian<quint32>(quint32(payload.size() + 1), result.data());
    result.append(char(kind));
    result.append(payload);
    return result;
}

QByteArray Wire::control(const QJsonObject &object)
{
    return pack(Kind::Control, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

Wire::ParseResult Wire::take(QByteArray &buffer, Frame &frame, QString &error)
{
    if (buffer.size() < 4)
        return ParseResult::Incomplete;
    const auto length = qFromBigEndian<quint32>(buffer.constData());
    if (length < 1 || length > MaxFrame) {
        error = QStringLiteral("非法帧长度");
        return ParseResult::Invalid;
    }
    if (buffer.size() < qsizetype(length) + 4)
        return ParseResult::Incomplete;
    const auto kind = quint8(buffer.at(4));
    if (kind != quint8(Kind::Control) && kind != quint8(Kind::Data)) {
        error = QStringLiteral("未知帧类型");
        return ParseResult::Invalid;
    }
    frame.kind = Kind(kind);
    frame.payload = buffer.mid(5, length - 1);
    buffer.remove(0, length + 4);
    return ParseResult::Complete;
}

bool Wire::safeFileName(const QString &name)
{
    if (name.isEmpty() || name.size() > 200 || name == "." || name == ".."
        || name.endsWith('.') || name.endsWith(' '))
        return false;
    for (const QChar c : name) {
        if (c.unicode() < 32 || QStringLiteral("/\\:*?\"<>|").contains(c))
            return false;
    }
    const QString stem = name.section('.', 0, 0).toUpper();
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"));
    return !reserved.match(stem).hasMatch();
}

bool Wire::validDigest(const QString &digest)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(digest).hasMatch();
}

bool Wire::parseNumber(const QJsonObject &object, const QString &key, qint64 &value)
{
    // Decimal strings preserve values beyond JSON's 53-bit integer range.
    const auto item = object.value(key);
    if (!item.isString())
        return false;
    bool ok = false;
    value = item.toString().toLongLong(&ok);
    return ok && value >= 0 && value <= MaxFileSize;
}

QString Wire::storageKey(const QString &name, qint64 size, const QString &digest)
{
    const QByteArray identity = name.toUtf8() + '\0' + QByteArray::number(size) + '\0' + digest.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QString Wire::readableSize(qint64 size)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double number = double(size);
    int unit = 0;
    while (number >= 1024 && unit < 4) { number /= 1024; ++unit; }
    return QStringLiteral("%1 %2").arg(number, 0, 'f', unit ? 1 : 0).arg(QString::fromLatin1(units[unit]));
}
