#include "discovery.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>

Discovery::Discovery(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<Peer>();
    m_timer.setInterval(3000);
    connect(&m_timer, &QTimer::timeout, this, &Discovery::heartbeat);
    connect(&m_socket, &QUdpSocket::readyRead, this, &Discovery::receive);
}

bool Discovery::start(const QString &id, const QString &name, quint16 tcpPort, quint16 udpPort)
{
    stop();
    m_id = id;
    m_name = name.left(80);
    m_tcpPort = tcpPort;
    m_udpPort = udpPort;
    if (!m_socket.bind(QHostAddress::AnyIPv4, udpPort,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit error(QStringLiteral("设备发现启动失败：%1").arg(m_socket.errorString()));
        return false;
    }
    m_timer.start();
    probe();
    return true;
}

void Discovery::stop()
{
    if (m_socket.state() == QAbstractSocket::BoundState)
        announce(QStringLiteral("bye"), QHostAddress::Broadcast, m_udpPort);
    m_timer.stop();
    m_socket.close();
    m_peers.clear();
}

void Discovery::announce(const QString &type, const QHostAddress &address, quint16 destinationPort)
{
    const QJsonObject object{{"app", "ByteMe"}, {"version", 1}, {"type", type},
                             {"id", m_id}, {"name", m_name}, {"port", int(m_tcpPort)}};
    m_socket.writeDatagram(QJsonDocument(object).toJson(QJsonDocument::Compact), address, destinationPort);
}

void Discovery::probe(const QHostAddress &address)
{
    if (m_socket.state() != QAbstractSocket::BoundState)
        return;
    if (address != QHostAddress::Broadcast) {
        announce(QStringLiteral("hello"), address, m_udpPort);
        return;
    }
    QSet<QString> broadcasts{QHostAddress(QHostAddress::Broadcast).toString()};
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp)
            || !(interface.flags() & QNetworkInterface::CanBroadcast))
            continue;
        for (const auto &entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull())
                broadcasts.insert(entry.broadcast().toString());
        }
    }
    for (const auto &broadcast : broadcasts)
        announce(QStringLiteral("hello"), QHostAddress(broadcast), m_udpPort);
}

void Discovery::heartbeat()
{
    probe();
    const auto now = QDateTime::currentDateTimeUtc();
    for (auto it = m_peers.begin(); it != m_peers.end();) {
        if (it->lastSeen.secsTo(now) > 12) {
            emit peerGone(it.key());
            it = m_peers.erase(it);
        } else {
            ++it;
        }
    }
}

void Discovery::receive()
{
    // Bounded datagrams and batches prevent a busy network from starving the UI.
    int budget = 64;
    while (m_socket.hasPendingDatagrams() && budget-- > 0) {
        const auto datagram = m_socket.receiveDatagram(4096);
        if (!datagram.isValid() || datagram.data().size() > 2048)
            continue;
        const auto document = QJsonDocument::fromJson(datagram.data());
        if (!document.isObject())
            continue;
        const auto object = document.object();
        const auto id = object.value("id").toString();
        const auto name = object.value("name").toString();
        const int port = object.value("port").toInt();
        const auto type = object.value("type").toString();
        if (object.value("app") != "ByteMe" || object.value("version").toInt() != 1
            || id.isEmpty() || id.size() > 80 || id == m_id || name.size() > 80
            || port < 1 || port > 65535 || (type != "hello" && type != "alive" && type != "bye"))
            continue;
        if (type == "bye") {
            m_peers.remove(id);
            emit peerGone(id);
            continue;
        }
        if (m_peers.size() >= 256 && !m_peers.contains(id))
            continue;
        Peer peer{id, name, datagram.senderAddress(), quint16(port), QDateTime::currentDateTimeUtc()};
        m_peers.insert(id, peer);
        emit peerSeen(peer);
        if (type == "hello")
            announce(QStringLiteral("alive"), datagram.senderAddress(), datagram.senderPort());
    }
    if (m_socket.hasPendingDatagrams())
        QTimer::singleShot(0, this, &Discovery::receive);
}
