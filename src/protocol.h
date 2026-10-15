#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace Wire {
constexpr quint16 DiscoveryPort = 45454;
constexpr quint16 TransferPort = 45455;
constexpr int ChunkSize = 64 * 1024;
constexpr int MaxFrame = 256 * 1024;
constexpr qint64 MaxFileSize = 1024LL * 1024 * 1024 * 1024; // 1 TiB
enum class Kind : quint8 { Control = 1, Data = 2 };
enum class ParseResult { Incomplete, Complete, Invalid };
struct Frame { Kind kind = Kind::Control; QByteArray payload; };
QByteArray pack(Kind kind, const QByteArray &payload);
QByteArray control(const QJsonObject &object);
ParseResult take(QByteArray &buffer, Frame &frame, QString &error);
bool safeFileName(const QString &name);
bool validDigest(const QString &digest);
bool parseNumber(const QJsonObject &object, const QString &key, qint64 &value);
QString storageKey(const QString &name, qint64 size, const QString &digest);
QString readableSize(qint64 size);
}
