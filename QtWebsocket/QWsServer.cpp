/*
Copyright 2013 Antoine Lafarge qtwebsocket@gmail.com

This file is part of QtWebsocket.

QtWebsocket is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.

QtWebsocket is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with QtWebsocket.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "QWsServer.h"

#include <QStringList>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>

namespace QtWebsocket
{

QWebSocketServer::QWebSocketServer(QObject* parent, Protocol allowedProtocols)
	: QObject(parent),
	tcpServer(new QTcpServer(this)),
	tlsServer(this, allowedProtocols)
{
	if (allowedProtocols & Tls)
	{
		tcpServer = &tlsServer;
		QObject::connect(tcpServer, SIGNAL(newTlsConnection(QSslSocket*)), this, SLOT(newTlsConnection(QSslSocket*)));
	}
	else
	{
		QObject::connect(tcpServer, SIGNAL(newConnection()), this, SLOT(newTcpConnection()));
	}
}

QWebSocketServer::~QWebSocketServer()
{
	tcpServer->deleteLater();
}

bool QWebSocketServer::listen(const QHostAddress & address, quint16 port)
{
	return tcpServer->listen(address, port);
}

void QWebSocketServer::close()
{
	tcpServer->close();
}

Protocol QWebSocketServer::allowedProtocols()
{
	return tlsServer.allowedProtocols();
}

QAbstractSocket::SocketError QWebSocketServer::serverError()
{
	return tcpServer->serverError();
}

QString QWebSocketServer::errorString()
{
	return tcpServer->errorString();
}

void QWebSocketServer::newTcpConnection()
{
	QTcpSocket* tcpSocket = tcpServer->nextPendingConnection();
	if (tcpSocket == NULL)
	{
		return;
	}
	QObject::connect(tcpSocket, SIGNAL(readyRead()), this, SLOT(dataReceived()));
	QObject::connect(tcpSocket, SIGNAL(disconnected()), this, SLOT(tcpSocketDisconnected()));
	handshakeBuffer.insert(tcpSocket, new QWsHandshake(WsClientMode));
}

void QWebSocketServer::newTlsConnection(QSslSocket* serverSocket)
{
	if (serverSocket == NULL)
	{
		return;
	}
	QObject::connect(serverSocket, SIGNAL(readyRead()), this, SLOT(dataReceived()));
	QObject::connect(serverSocket, SIGNAL(disconnected()), this, SLOT(tcpSocketDisconnected()));
	handshakeBuffer.insert(serverSocket, new QWsHandshake(WsClientMode));
}

void QWebSocketServer::tcpSocketDisconnected()
{
	QTcpSocket* tcpSocket = qobject_cast<QTcpSocket*>(sender());
	if (tcpSocket == NULL)
	{
		return;
	}
	
	QWsHandshake* handshake = handshakeBuffer.take(tcpSocket);
	delete handshake;
	tcpSocket->deleteLater();
}

void QWebSocketServer::closeTcpConnection()
{
	QTcpSocket* tcpSocket = qobject_cast<QTcpSocket*>(sender());
	if (tcpSocket == NULL)
	{
		return;
	}
	
	tcpSocket->close();
}

static void showErrorAndClose(QTcpSocket* tcpSocket)
{
	// Send bad request response
	QString response = QWebSocketServer::composeBadRequestResponse(QList<EWebsocketVersion>() << WS_V6 << WS_V7 << WS_V8 << WS_V13);
	tcpSocket->write(response.toUtf8());
	tcpSocket->flush();
	tcpSocket->close();
}

void QWebSocketServer::dataReceived()
{
	QTcpSocket* tcpSocket = qobject_cast<QTcpSocket*>(sender());
	if (tcpSocket == NULL)
	{
		return;
	}

	QWsHandshake& handshake = *(handshakeBuffer.value(tcpSocket));

	if (handshake.read(tcpSocket) == false)
	{
		showErrorAndClose(tcpSocket);
		return;
	}

	// handshake complete
	if (!handshake.readStarted || !handshake.complete)
	{
		if (handshake.readStarted && !handshake.httpRequestValid)
		{
			showErrorAndClose(tcpSocket);
		}
		return;
	}

	// If the mandatory fields are not specified, we abord the connection to the Websocket server
	// hansake valid
	if (!handshake.isValid())
	{
		showErrorAndClose(tcpSocket);
		return;
	}
	
	// Handshake fully parsed
	QObject::disconnect(tcpSocket, SIGNAL(readyRead()), this, SLOT(dataReceived()));
	QObject::disconnect(tcpSocket, SIGNAL(disconnected()), this, SLOT(tcpSocketDisconnected()));

	// Compose opening handshake response
	QByteArray handshakeResponse;

	if (handshake.version >= WS_V6)
	{
		QByteArray accept = QWebSocket::computeAcceptV4(handshake.key);
		handshakeResponse = QWebSocketServer::composeOpeningHandshakeResponseV6(accept, handshake.protocol).toUtf8();
	}
	else if (handshake.version >= WS_V4)
	{
		QByteArray accept = QWebSocket::computeAcceptV4(handshake.key);
		QByteArray nonce = QWebSocket::generateNonce();
		handshakeResponse = QWebSocketServer::composeOpeningHandshakeResponseV4(accept, nonce, handshake.protocol).toUtf8();
	}
	else // version WS_V0
	{
		QByteArray accept = QWebSocket::computeAcceptV0(handshake.key1, handshake.key2, handshake.key3);
		// safari 5.1.7 don't accept the utf8 charset here...
		handshakeResponse = QWebSocketServer::composeOpeningHandshakeResponseV0(accept, handshake.origin, handshake.hostAddress, handshake.hostPort, handshake.resourceName , handshake.protocol).toLatin1();
	}
	
	// Send opening handshake response
	tcpSocket->write(handshakeResponse);
	tcpSocket->flush();

	QWebSocket* wsSocket = new QWebSocket(this, tcpSocket, handshake.version);
	wsSocket->setResourceName(handshake.resourceName);
	wsSocket->setHost(handshake.host);
	wsSocket->setHostAddress(handshake.hostAddress);
	wsSocket->setHostPort(handshake.hostPort.toUInt());
	wsSocket->setOrigin(handshake.origin);
	wsSocket->setProtocol(handshake.protocol);
	wsSocket->setExtensions(handshake.extensions);
	wsSocket->setWsMode(WsServerMode);
	
	QWsHandshake* hsTmp = handshakeBuffer.take(tcpSocket);
	delete hsTmp;

	// CAN'T DO THAT WITHOUT DISCONNECTING THE QTcpSocket
	//int socketDescriptor = tcpSocket->socketDescriptor();
	//incomingConnection(socketDescriptor);	
	// USE THIS INSTEAD
	addPendingConnection(wsSocket);
	emit newConnection();
}

void QWebSocketServer::incomingConnection(int socketDescriptor)
{
	QTcpSocket* tcpSocket = new QTcpSocket(tcpServer);
	tcpSocket->setSocketDescriptor(socketDescriptor, QAbstractSocket::ConnectedState);
	QWebSocket* wsSocket = new QWebSocket(this, tcpSocket);

	addPendingConnection(wsSocket);
	emit newConnection();
}

void QWebSocketServer::addPendingConnection(QWebSocket* socket)
{
	if (pendingConnections.size() < maxPendingConnections())
	{
		pendingConnections.enqueue(socket);
	}
}

QWebSocket* QWebSocketServer::nextPendingConnection()
{
	return pendingConnections.dequeue();
}

bool QWebSocketServer::hasPendingConnections()
{
	if (pendingConnections.size() > 0)
	{
		return true;
	}
	return false;
}

int QWebSocketServer::maxPendingConnections()
{
	return tcpServer->maxPendingConnections();
}

bool QWebSocketServer::isListening()
{
	return tcpServer->isListening();
}

QNetworkProxy QWebSocketServer::proxy()
{
	return tcpServer->proxy();
}

QHostAddress QWebSocketServer::serverAddress()
{
	return tcpServer->serverAddress();
}

quint16 QWebSocketServer::serverPort()
{
	return tcpServer->serverPort();
}

void QWebSocketServer::setMaxPendingConnections(int numConnections)
{
	tcpServer->setMaxPendingConnections(numConnections);
}

void QWebSocketServer::setProxy(const QNetworkProxy & networkProxy)
{
	tcpServer->setProxy(networkProxy);
}

bool QWebSocketServer::setSocketDescriptor(int socketDescriptor)
{
	return tcpServer->setSocketDescriptor(socketDescriptor);
}

int QWebSocketServer::socketDescriptor()
{
	return tcpServer->socketDescriptor();
}

bool QWebSocketServer::waitForNewConnection(int msec, bool* timedOut)
{
	return tcpServer->waitForNewConnection(msec, timedOut);
}

QString QWebSocketServer::composeOpeningHandshakeResponseV0(QByteArray accept, QString origin, QString hostAddress, QString hostPort, QString resourceName, QString protocol)
{
	QString response;
	response += QLatin1String("HTTP/1.1 101 WebSocket Protocol Handshake\r\n");
	response += QLatin1String("Upgrade: Websocket\r\n");
	response += QLatin1String("Connection: Upgrade\r\n");
	response += QString("Sec-WebSocket-Origin: %1\r\n").arg(origin);
	if (!hostAddress.startsWith("ws://", Qt::CaseInsensitive))
	{
		hostAddress.prepend(QLatin1String("ws://"));
	}
	if (!hostPort.isEmpty())
	{
		hostPort.prepend(QLatin1Char(':'));
	}
	response += QString("Sec-WebSocket-Location: %1%2%3\r\n").arg(hostAddress).arg(hostPort).arg(resourceName);
	if (!protocol.isEmpty())
	{
		response += QString("Sec-WebSocket-Protocol: %1\r\n").arg(protocol);
	}
	response += QLatin1String("\r\n");
	response += QLatin1String(accept);

	return response;
}

QString QWebSocketServer::composeOpeningHandshakeResponseV4(QByteArray accept, QByteArray nonce, QString protocol, QString extensions)
{
	QString response;
	response += QLatin1String("HTTP/1.1 101 WebSocket Protocol Handshake\r\n");
	response += QLatin1String("Upgrade: websocket\r\n");
	response += QLatin1String("Connection: Upgrade\r\n");
	response += QString("Sec-WebSocket-Accept: %1\r\n").arg(QLatin1String(accept));
	response += QString("Sec-WebSocket-Nonce: %1\r\n").arg(QLatin1String(nonce));
	if (!protocol.isEmpty())
	{
		response += QString("Sec-WebSocket-Protocol: %1\r\n").arg(protocol);
	}
	if (!extensions.isEmpty())
	{
		response += QString("Sec-WebSocket-Extensions: %1\r\n").arg(extensions);
	}
	response += QLatin1String("\r\n");
	return response;
}

QString QWebSocketServer::composeOpeningHandshakeResponseV6(QByteArray accept, QString protocol, QString extensions)
{
	QString response;
	response += QLatin1String("HTTP/1.1 101 WebSocket Protocol Handshake\r\n");
	response += QLatin1String("Upgrade: websocket\r\n");
	response += QLatin1String("Connection: Upgrade\r\n");
	response += QString("Sec-WebSocket-Accept: %1\r\n").arg(QLatin1String(accept));
	if (!protocol.isEmpty())
	{
		response += QString("Sec-WebSocket-Protocol: %1\r\n").arg(protocol);
	}
	if (!extensions.isEmpty())
	{
		response += QString("Sec-WebSocket-Extensions: %1\r\n").arg(extensions);
	}
	response += QLatin1String("\r\n");
	return response;
}

QString QWebSocketServer::composeBadRequestResponse(QList<EWebsocketVersion> versions)
{
	QString response;
	response += QLatin1String("HTTP/1.1 400 Bad Request\r\n");
	if (!versions.isEmpty())
	{
		QString versionsStr;
		int i = versions.size();
		while (i--)
		{
			versionsStr += QString::number((quint16)versions.at(i));
			if (i)
			{
				versionsStr += QLatin1String(", ");
			}
		}
		response += QString("Sec-WebSocket-Version: %1\r\n").arg(versionsStr);
	}
	return response;
}

} // namespace QtWebsocket
