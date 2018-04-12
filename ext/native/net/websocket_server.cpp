#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <unistd.h>
#else
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "base/stringutil.h"
#include "data/base64.h"
#include "net/http_server.h"
#include "net/sinks.h"
#include "net/websocket_server.h"
// TODO: Not a great cross dependency.
#include "Common/Crypto/sha1.h"

#ifdef _WIN32
// Function Cross-Compatibility
#define strcasecmp _stricmp
#endif

static const char *const WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

namespace net {

enum class Opcode {
	CONTINUE = 0,
	TEXT = 1,
	BINARY = 2,

	CLOSE = 8,
	PING = 9,
	PONG = 10,

	PAYLOAD_MAX = 2,
	CONTROL_MIN = 8,
	CONTROL_MAX = 10,
};

WebSocketServer *WebSocketServer::CreateAsUpgrade(const http::Request &request) {
	auto requireHeader = [&](const char *name, const char *expected) {
		std::string val;
		if (!request.GetHeader(name, &val)) {
			return false;
		}
		return strcasecmp(val.c_str(), expected) == 0;
	};
	auto requireHeaderContains = [&](const char *name, const char *expected) {
		std::string val;
		if (!request.GetHeader(name, &val)) {
			return false;
		}
		std::transform(val.begin(), val.end(), val.begin(), tolower);
		return strstr(val.c_str(), expected) != 0;
	};

	if (!requireHeader("upgrade", "websocket") || !requireHeaderContains("connection", "upgrade")) {
		request.WriteHttpResponseHeader(400, -1, "text/plain");
		request.Out()->Push("Must send a websocket request.");
		return nullptr;
	}
	if (!requireHeader("sec-websocket-version", "13")) {
		request.WriteHttpResponseHeader(400, -1, "text/plain", "Sec-WebSocket-Version: 13\r\n");
		request.Out()->Push("Unsupported version.");
		return nullptr;
	}

	std::string key;
	if (!request.GetHeader("sec-websocket-key", &key)) {
		request.WriteHttpResponseHeader(400, -1, "text/plain");
		request.Out()->Push("Cannot accept without key.");
		return nullptr;
	}

	key += WEBSOCKET_GUID;
	unsigned char accept[20];
	sha1((unsigned char *)key.c_str(), (int)key.size(), accept);

	std::string acceptKey = Base64Encode(accept, 20);
	std::string otherHeaders = StringFromFormat("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n", acceptKey.c_str());

	// Okay, we're good to go then.
	request.WriteHttpResponseHeader(101, -1, "websocket", otherHeaders.c_str());
	request.WritePartial();

	return new WebSocketServer(request.fd(), request.In(), request.Out());
}

void WebSocketServer::Send(const std::string &str) {
	assert(open_);
	SendHeader(true, (int)Opcode::TEXT, str.size());
	// TODO: For long strings, this will block.  Possibly not ideal.
	if (!out_->Push(str.c_str(), str.size())) {
		assert(false);
		open_ = false;
		closeReason_ = WebSocketClose::ABNORMAL;
	}
}

void WebSocketServer::Send(const std::vector<uint8_t> &payload) {
	assert(open_);
	SendHeader(true, (int)Opcode::BINARY, payload.size());
	// TODO: For long strings, this will block.  Possibly not ideal.
	if (!out_->Push((const char *)&payload[0], payload.size())) {
		assert(false);
		open_ = false;
		closeReason_ = WebSocketClose::ABNORMAL;
	}
}

void WebSocketServer::Ping(const std::vector<uint8_t> &payload) {
	assert(open_);
	assert(payload.size() <= 125);
	SendHeader(true, (int)Opcode::PING, payload.size());
	if (!out_->Push((const char *)&payload[0], payload.size())) {
		assert(false);
		open_ = false;
		closeReason_ = WebSocketClose::ABNORMAL;
	}
}

void WebSocketServer::Pong(const std::vector<uint8_t> &payload) {
	assert(open_);
	assert(payload.size() <= 125);
	SendHeader(true, (int)Opcode::PONG, payload.size());
	if (!out_->Push((const char *)&payload[0], payload.size())) {
		assert(false);
		open_ = false;
		closeReason_ = WebSocketClose::ABNORMAL;
	}
}

void WebSocketServer::Close(WebSocketClose reason) {
	closeReason_ = reason;
	SendHeader(true, (int)Opcode::CLOSE, 2);

	uint16_t r = (uint16_t)reason;
	uint8_t reasonData[] = {
		(uint8_t)((r >> 8) & 0xFF),
		(uint8_t)((r >> 0) & 0xFF),
	};
	if (!out_->Push((const char *)reasonData, sizeof(reasonData))) {
		assert(false);
		open_ = false;
		closeReason_ = WebSocketClose::ABNORMAL;
	}

	sentClose_ = true;
}

bool WebSocketServer::Process(float timeout) {
	if (!open_) {
		return false;
	}

	out_->Flush(false);

	if (out_->Empty() && sentClose_) {
		// Okay, we've sent the close.  Don't wait for anything else (whether we got a close or not.)
		open_ = false;
		return false;
	}

	struct timeval tv;
	tv.tv_sec = floor(timeout);
	tv.tv_usec = (timeout - floor(timeout)) * 1000000.0;

	fd_set read;
	FD_ZERO(&read);
	// In case we closed due to protocol error, don't even try to read.
	if (!sentClose_) {
		FD_SET(fd_, &read);
	}

	fd_set write;
	FD_ZERO(&write);
	if (!out_->Empty()) {
		FD_SET(fd_, &write);
	}

	// First argument to select is the highest socket in the set + 1.
	int rval = select((int)fd_ + 1, &read, &write, nullptr, &tv);
	if (rval < 0) {
		// Something went wrong with the select() call.
		// TODO: Could be EINTR, for now returning true...
		return true;
	}

	if (rval == 0) {
		// Timed out.
		return true;
	}

	if (FD_ISSET(fd_, &write)) {
		out_->Flush(false);
	}
	if (FD_ISSET(fd_, &read)) {
		if (in_->Empty() && !in_->TryFill()) {
			// Since select said it was readable, we assume this means disconnect.
			closeReason_ = WebSocketClose::ABNORMAL;
			open_ = false;
			return false;
		}

		while (ReadFrames() && !in_->Empty())
			continue;
	}

	return true;
}

bool WebSocketServer::ReadFrames() {
	if (pendingLeft_ != 0) {
		return ReadPending();
	}

	return ReadFrame();
}

bool WebSocketServer::ReadFrame() {
	assert(pendingLeft_ == 0);

	// TODO: For now blocking on header trickle, shouldn't be common.
	auto readExact = [&](void *p, size_t sz) {
		if (!in_->TakeExact((char *)p, sz)) {
			// TODO: Failing on too slow trickle timeout for now.
			Close(WebSocketClose::POLICY_VIOLATION);
			return false;
		}
		return true;
	};

	// Client frames are always between 6 and 14 bytes.  We start with 6.
	uint8_t header[14];
	if (!readExact(header, 6))
		return false;

	// Don't allow reserved bits to be set, require masking.
	if ((header[0] & 0x70) != 0 || (header[1] & 0x80) == 0) {
		Close(WebSocketClose::PROTOCOL_ERROR);
		return false;
	}

	const bool fin = (header[0] & 0x80) != 0;
	const int opcode = header[0] & 0x0F;
	uint64_t sz = header[1] & 0x7F;
	const uint8_t *mask = &header[2];

	if (opcode >= (int)Opcode::CONTROL_MIN && (sz > 125 || !fin)) {
		// Control frames must be <= 125 bytes.
		Close(WebSocketClose::PROTOCOL_ERROR);
		return false;
	}

	if (opcode > (int)Opcode::CONTROL_MAX || (opcode > (int)Opcode::PAYLOAD_MAX && opcode < (int)Opcode::CONTROL_MIN)) {
		// Undefined opcode.
		Close(WebSocketClose::PROTOCOL_ERROR);
		return false;
	}

	if (!pendingFin_ && opcode == (int)Opcode::CONTINUE) {
		// Can't continue what you haven't started.
		Close(WebSocketClose::PROTOCOL_ERROR);
		return false;
	}
	if (pendingFin_ && opcode != (int)Opcode::CONTINUE && opcode < (int)Opcode::CONTROL_MIN) {
		// Can't start something else until you finish your thought.
		Close(WebSocketClose::PROTOCOL_ERROR);
		return false;
	}

	if (sz == 126) {
		// Read the rest of the mask.
		if (!readExact((char *)&header[6], 2))
			return false;

		mask = &header[4];
		sz = (header[2] << 8) | (header[3] << 0);
	} else if (sz == 127) {
		// We only have half the size so far - read the rest, and the mask.
		if (!readExact((char *)&header[6], 8))
			return false;

		mask = &header[10];
		// Read from big endian.
		uint64_t high = (header[2] << 24) | (header[3] << 16) || (header[4] << 8) | (header[5] << 0);
		uint64_t low = (header[6] << 24) | (header[7] << 16) || (header[8] << 8) | (header[9] << 0);
		sz = (high << 32) | low;

		if ((sz & 0x8000000000000000ULL) != 0) {
			Close(WebSocketClose::PROTOCOL_ERROR);
			return false;
		}
	}

	if (opcode >= (int)Opcode::CONTROL_MIN) {
		// It's safe to overwrite this since we can be between fragmented frames, but not inside a frame.
		memcpy(pendingMask_, mask, sizeof(pendingMask_));
		return ReadControlFrame(opcode, sz);
	}

	// The data could be split among many TCP packets, so read it as it comes.
	if (!pendingFin_)
		pendingOpcode_ = opcode;
	pendingFin_ = !fin;
	pendingLeft_ = sz;
	memcpy(pendingMask_, mask, sizeof(pendingMask_));

	// Payload data is actually read in ReadPending().
	return true;
}

bool WebSocketServer::ReadPending() {
	size_t pos = pendingBuf_.size();
	pendingBuf_.resize(pendingBuf_.size() + pendingLeft_);

	// Read what we can.
	size_t readBytes = in_->TakeAtMost((char *)&pendingBuf_[pos], pendingLeft_);
	for (size_t i = 0; i < readBytes; ++i) {
		pendingBuf_[pos + i] ^= pendingMask_[i & 3];
	}
	pendingLeft_ -= readBytes;

	if (pendingLeft_ != 0) {
		// Still more to read.  Careful: we might need to rotate the mask.
		// Example: if we read only 3 bytes, next read should start at fourth byte in mask.
		int offset = readBytes & 3;
		if (offset) {
			uint8_t orig[4];
			memcpy(orig, pendingMask_, sizeof(orig));
			for (size_t i = 0; i < sizeof(orig); ++i) {
				pendingMask_[i] = orig[(offset + i) & 3];
			}
		}

		// Truncate out the unread bytes for next time.
		pendingBuf_.resize(pos + readBytes);
		return true;
	}

	// We're done, but were we waiting for a FIN packet?
	if (pendingFin_)
		return true;

	if (pendingOpcode_ == (int)Opcode::TEXT) {
		if (text_) {
			text_(std::string(pendingBuf_.begin(), pendingBuf_.end()));
		}
	} else if (pendingOpcode_ == (int)Opcode::BINARY) {
		if (binary_) {
			binary_(pendingBuf_);
		}
	} else {
		assert(false);
	}

	// All done, clear it out.
	pendingBuf_.clear();
	pendingOpcode_ = -1;

	return true;
}

bool WebSocketServer::ReadControlFrame(int opcode, size_t sz) {
	std::vector<uint8_t> payload;
	payload.resize(sz);
	// Just block here to read the payload.
	if (!in_->TakeExact((char *)&payload[0], sz)) {
		// TODO: Failing on too slow trickle timeout for now.
		Close(WebSocketClose::POLICY_VIOLATION);
		return false;
	}

	for (size_t i = 0; i < sz; ++i) {
		payload[i] ^= pendingMask_[i & 3];
	}

	if (opcode == (int)Opcode::PING) {
		Pong(payload);
		// Try to send immediately if possible, but don't block.
		out_->Flush(false);

		if (ping_) {
			ping_(payload);
		}
	} else if (opcode == (int)Opcode::PONG) {
		if (pong_) {
			pong_(payload);
		}
	} else if (opcode == (int)Opcode::CLOSE) {
		if (payload.size() >= 2) {
			uint16_t reason = (payload[0] << 8) | payload[1];
			// Send back a close right away.
			Close(WebSocketClose(reason));
		} else {
			Close(WebSocketClose::NO_STATUS);
		}
		// Don't read anything more.
		return false;
	} else {
		assert(false);
	}

	return true;
}

void WebSocketServer::SendHeader(bool fin, int opcode, size_t sz) {
	assert((opcode & 0x0F) == opcode);
	uint8_t frameHeader = (fin ? 0x80 : 0x00) | opcode;
	out_->Push((const char *)&frameHeader, 1);

	// We never mask from the server.
	if (sz <= 125) {
		uint8_t frameSize = (int8_t)sz;
		out_->Push((const char *)&frameSize, 1);
	} else if (sz <= 0xFFFF) {
		uint8_t frameSize[] = {
			126,
			(uint8_t)((sz >> 8) & 0xFF),
			(uint8_t)((sz >> 0) & 0xFF),
		};
		out_->Push((const char *)frameSize, sizeof(frameSize));
	} else {
		uint64_t sz64 = sz;
		assert((sz64 & 0x8000000000000000ULL) == 0);
		uint8_t frameSize[] = {
			127,
			(uint8_t)((sz64 >> 56) & 0xFF),
			(uint8_t)((sz64 >> 48) & 0xFF),
			(uint8_t)((sz64 >> 40) & 0xFF),
			(uint8_t)((sz64 >> 32) & 0xFF),
			(uint8_t)((sz64 >> 24) & 0xFF),
			(uint8_t)((sz64 >> 16) & 0xFF),
			(uint8_t)((sz64 >> 8) & 0xFF),
			(uint8_t)((sz64 >> 0) & 0xFF),
		};
		out_->Push((const char *)frameSize, sizeof(frameSize));
	}
}

};
