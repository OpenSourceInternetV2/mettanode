/*
 * Structured Stream Transport
 * Copyright (C) 2006-2008 Massachusetts Institute of Technology
 * Author: Bryan Ford
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include <QHostInfo>
#include <QtDebug>

#include "reginfo.h"
#include "regcli.h"
#include "ident.h"
#include "sock.h"
#include "host.h"
#include "util.h"
#include "crypto/sha2.h"
#include "xdr.h"

using namespace SST;

//=====================================================================================================================
// RegClient
//=====================================================================================================================

const qint64 RegClient::maxRereg;

QString RegClient::stateString(int state)
{
	switch (state)
	{
		case Idle: return "Unregistered and not doing anything";
		case Resolve: return "Resolving rendezvous server's host name";
		case Insert1: return "Sent Insert1 request, waiting response";
		case Insert2: return "Sent Insert2 request, waiting response";
		case Registered: return "Successfully registered";
		default: return "Unknown registration client state";
	}
}

RegClient::RegClient(Host *h, QObject *parent)
:	QObject(parent),
	h(h),
	state(Idle),
	idi(h->hostIdent().id()),
	retrytimer(h),
	reregtimer(h)
{
	connect(&retrytimer, SIGNAL(timeout(bool)),
		this, SLOT(timeout(bool)));
	connect(&reregtimer, SIGNAL(timeout(bool)),
		this, SLOT(reregTimeout()));

	h->regClientSet.insert(this);
	h->regClientCreate(this);
}

RegClient::~RegClient()
{
	// First disconnect and cancel any outstanding lookups.
	disconnect();

	// Notify anyone interested of our upcoming destruction.
	h->regClientDestroy(this);
	h->regClientSet.remove(this);

	// Remove any nonce we may have registered in the nhihash.
	h->rcvr.nhihash.remove(nhi);
}

void RegClient::fail(const QString &err)
{
	this->err = err;
	disconnect();
}

void RegClient::disconnect()
{
	if (state == Idle)
		return;

	qDebug() << this << "disconnect";
	sendDelete();

	// Fail all outstanding lookup and search requests
	// XX provide a better error indication?
	foreach (const PeerId &id, lookups)
		lookupDone(id, Endpoint(), RegInfo());
	foreach (const PeerId &id, punches)
		lookupDone(id, Endpoint(), RegInfo());
	foreach (const QString &text, searches)
		searchDone(text, QList<SST::PeerId>(), true);

	state = Idle;
	addrs.clear();
	ni.clear();
	nhi.clear();
	chal.clear();
	key.clear();
	sig.clear();
	lookups.clear();
	punches.clear();
	searches.clear();
	retrytimer.stop();
	reregtimer.stop();

	// Notify the user that we're not (or no longer) registered
	stateChanged(state);
}

void RegClient::registerAt(const QString &srvname, quint16 srvport)
{
	Q_ASSERT(state == Idle);
	qDebug() << this << "registerAt" << srvname << srvport;

	this->srvname = srvname;
	this->srvport = srvport;
	reregister();
}

void RegClient::reregister()
{
	qDebug() << this << "reregister";

	Q_ASSERT(!srvname.isEmpty());
	Q_ASSERT(srvport != 0);

	// Clear any previous nonce we may have used
	if (!ni.isEmpty()) {
		h->rcvr.nhihash.remove(nhi);
		ni.clear();
		nhi.clear();
	}

	// Lookup the server hostname
	QHostAddress addr(srvname);
	if (addr.isNull()) {

		// Need to try DNS resolution on the address.
		lookupid = QHostInfo::lookupHost(srvname, this,
					SLOT(resolveDone(QHostInfo)));
		state = Resolve;
		return;
	}

	// Just use the IP address we were given in string form
	this->addrs.clear();
	this->addrs.append(addr);

	goInsert1();
}

void RegClient::resolveDone(const QHostInfo &hi)
{
	addrs = hi.addresses();
	if (addrs.isEmpty())
		return fail(hi.errorString());
	qDebug() << this << "primary server address" << addrs[0].toString();

	goInsert1();
}

void RegClient::goInsert1()
{
	// Create our random nonce and its hash, if not done already,
	// and register this client to receive replies keyed on this nonce.
	if (ni.isEmpty()) {
		ni = randBytes(SHA256_DIGEST_LENGTH);
		nhi = Sha256::hash(ni);
		h->rcvr.nhihash.insert(nhi, this);
	}
	Q_ASSERT(ni.size() == SHA256_DIGEST_LENGTH);
	Q_ASSERT(nhi.size() == SHA256_DIGEST_LENGTH);

	// Enter Insert1 state and start sending
	state = Insert1;
	sendInsert1();
	retrytimer.start();
}

void RegClient::sendInsert1()
{
	qDebug() << this << "Insert1";

	// Send our Insert1 message
	QByteArray msg;
	XdrStream ws(&msg, QIODevice::WriteOnly);
	ws << (quint32)REG_MAGIC << (quint32)(REG_REQUEST | REG_INSERT1) << idi << nhi;
	send(msg);
}

void RegClient::gotInsert1Reply(XdrStream &rs)
{
	qDebug() << this << "Insert1 reply";

	// Decode the rest of the reply
	rs >> chal;
	if (rs.status() != rs.Ok) {
		qDebug() << this << "got invalid Insert1 reply";
		return;
	}

	// Looks good - go to Insert2 state.
	qDebug() << this << "Insert1 reply looks good!";
	goInsert2();
}

void RegClient::goInsert2()
{
	qDebug() << this << "Insert2";

	// Find our serialized public key to send to the server.
	Ident identi = h->hostIdent();
	key = identi.key();

	// Compute the hash of the message components to be signed.
	Sha256 sigsha;
	XdrStream sigws(&sigsha);
	sigws << idi << ni << chal << inf.encode();

	// Generate our signature.
	sig = identi.sign(sigsha.final());
	Q_ASSERT(!sig.isEmpty());

	state = Insert2;
	sendInsert2();
	retrytimer.start();
}

void RegClient::sendInsert2()
{
	qDebug() << this << "Insert2 reply";

	// Send our Insert2 message
	QByteArray msg;
	XdrStream ws(&msg, QIODevice::WriteOnly);
	ws << REG_MAGIC << (quint32)(REG_REQUEST | REG_INSERT2) << idi << ni << chal << inf.encode() << key << sig;
	send(msg);
}

void RegClient::gotInsert2Reply(XdrStream &rs)
{
	// Decode the rest of the reply
	qint32 lifeSecs;
	Endpoint pubEp;
	rs >> lifeSecs >> pubEp;
	if (rs.status() != rs.Ok) {
		qDebug() << this << "got invalid Insert2 reply";
		return;
	}

	// Looks good - consider ourselves registered.
	state = Registered;

	// Re-register when half the lifetime of our entry has expired.
	qint64 rereg = qMin((qint64)lifeSecs * 1000000 / 2, maxRereg);
	reregtimer.start(rereg);

	// Notify anyone interested.
	qDebug() << this << "registered with" << srvname << "for" << lifeSecs << "seconds";
	qDebug() << this << "my public endpoint" << pubEp.toString();
	stateChanged(state);
}

void RegClient::lookup(const PeerId& idtarget, bool notify)
{
	Q_ASSERT(registered());

	if (notify)
		punches.insert(idtarget);
	else
		lookups.insert(idtarget);
	sendLookup(idtarget, notify);
	retrytimer.start();
}

void RegClient::sendLookup(const PeerId& idtarget, bool notify)
{
	qDebug() << this << "send lookup for ID" << idtarget;

	// Prepare the Lookup message
	QByteArray msg;
	XdrStream ws(&msg, QIODevice::WriteOnly);
	ws << REG_MAGIC << (quint32)(REG_REQUEST | REG_LOOKUP) << idi << nhi << idtarget.getId() << notify;
	send(msg);
}

void RegClient::gotLookupReply(XdrStream &rs, bool isnotify)
{
	qDebug() << this << "gotLookupReply" << (isnotify ? "NOTIFY" : "RESPONSE");

	// Decode the rest of the reply
	QByteArray targetid, targetinfo;
	bool success;
	Endpoint targetloc;
	rs >> targetid >> success;
	if (success)
		rs >> targetloc >> targetinfo;
	if (rs.status() != rs.Ok) {
		qDebug() << this << "got invalid Lookup reply";
		return;
	}
	RegInfo reginfo(targetinfo);

	// If it's an async lookup notification from the server,
	// just forward it to anyone listening on our lookupNotify signal.
	if (isnotify)
		return lookupNotify(targetid, targetloc, reginfo);

	// Otherwise, it should be a response to a lookup request.
	if (!(lookups.contains(targetid) || punches.contains(targetid))) {
		qDebug() << this << "useless Lookup result";
		return;
	}
	qDebug() << this << "processed Lookup for" << PeerId(targetid);
	lookups.remove(targetid);
	punches.remove(targetid);
	lookupDone(targetid, targetloc, reginfo);
}

void RegClient::search(const QString &text)
{
	Q_ASSERT(registered());

	searches.insert(text);
	sendSearch(text);
	retrytimer.start();
}

void RegClient::sendSearch(const QString &text)
{
	// Prepare the Lookup message
	QByteArray msg;
	XdrStream ws(&msg, QIODevice::WriteOnly);
	ws << REG_MAGIC << (quint32)(REG_REQUEST | REG_SEARCH) << idi << nhi << text;
	send(msg);
}

void RegClient::gotSearchReply(XdrStream &rs)
{
	// Decode the first part of the reply
	QString text;
	bool complete;
	qint32 nresults;
	rs >> text >> complete >> nresults;
	if (rs.status() != rs.Ok || nresults < 0) {
		qDebug() << this << "got invalid Search reply";
		return;
	}

	// Make sure we actually did the indicated search
	if (!searches.contains(text)) {
		qDebug("RegClient: useless Search result");
		return;
	}

	// Decode the list of result IDs
	QList<SST::PeerId> ids;
	for (int i = 0; i < nresults; i++) {
		SST::PeerId id;
		rs >> id;
		if (rs.status() != rs.Ok) {
			qDebug() << this << "got invalid Search result ID";
			return;
		}
		ids.append(id);
	}

	searches.remove(text);
	searchDone(text, ids, complete);
}

void RegClient::sendDelete()
{
	qDebug() << this << "send delete notice";

	// Prepare the Delete message
	QByteArray msg;
	XdrStream ws(&msg, QIODevice::WriteOnly);
	ws << REG_MAGIC << (quint32)(REG_REQUEST | REG_DELETE) << idi << nhi;
	send(msg);
}

void RegClient::gotDeleteReply(XdrStream &)
{
	// Ignore.
	qDebug() << this << "got delete reply, ignored";
}

void RegClient::send(const QByteArray &msg)
{
	// Send the message to all addresses we know for the server,
	// using all of the currently active network sockets.
	// XXX should only do this during initial discovery!!
	QList<Socket*> socks = h->activeSockets();
	if (socks.isEmpty())
		qWarning() << this << "no active network sockets available";
	foreach (Socket *sock, socks)
		foreach (QHostAddress addr, addrs)
			sock->send(Endpoint(addr, srvport), msg);
}

void RegClient::timeout(bool failed)
{
	switch (state) {
	case Idle:
		break;
	case Resolve:
		break;
	case Insert1:
	case Insert2:
		if (failed && !persist) {
			fail("Timeout connecting to registration server");
		} else {
			if (state == Insert1)
				sendInsert1();
			else
				sendInsert2();
			retrytimer.restart();
		}
		break;
	case Registered:
		// Timeout on a Lookup or Search.
		if (lookups.isEmpty() && punches.isEmpty() && searches.isEmpty()) {
			// Nothing to do - don't bother with the timer.
			retrytimer.stop();
		} else if (failed) {
			// Our regserver is apparently no longer responding.
			// Disconnect (and try to re-connect if persistent).
			fail("Registration server no longer responding");
			if (persist)
				reregister();
		} else {
			// Re-send all outstanding requests
			foreach (const PeerId &id, lookups)
				sendLookup(id, false);
			foreach (const PeerId &id, punches)
				sendLookup(id, true);
			foreach (const QString &text, searches)
				sendSearch(text);
			retrytimer.restart();
		}
		break;
	}
}

void RegClient::reregTimeout()
{
	// Time to re-register!
	reregister();
}

//=====================================================================================================================
// RegReceiver
//=====================================================================================================================

RegReceiver::RegReceiver(Host *h)
:	SocketReceiver(h, REG_MAGIC)
{
}

void RegReceiver::receive(QByteArray &, XdrStream &rs,
				const SocketEndpoint &src)
{
	// Decode the first part of the message
	quint32 code;
	QByteArray nhi;
	rs >> code >> nhi;
	if (rs.status() != rs.Ok) {
		qDebug() << this << "received invalid message";
		return;
	}

	// Find the appropriate client
	RegClient *cli = nhihash.value(nhi);
	if (!cli) {
		qDebug() << this << "received message for nonexistent client";
		return;
	}

	// Make sure this message comes from one of the server's addresses
	if (cli->addrs.indexOf(src.addr) < 0 || src.port != cli->srvport) {
		qDebug() << this << "received message from wrong endpoint" << src;
		return;
	}

	// Dispatch it appropriately
	switch (code) {
	case REG_RESPONSE | REG_INSERT1:
		return cli->gotInsert1Reply(rs);
	case REG_RESPONSE | REG_INSERT2:
		return cli->gotInsert2Reply(rs);
	case REG_RESPONSE | REG_LOOKUP:
		return cli->gotLookupReply(rs, false);
	case REG_RESPONSE | REG_SEARCH:
		return cli->gotSearchReply(rs);
	case REG_RESPONSE | REG_DELETE:
		return cli->gotDeleteReply(rs);
	case REG_NOTIFY | REG_LOOKUP:
		return cli->gotLookupReply(rs, true);
	default:
		qDebug() << this << "bad message code" << code;
	}
}

