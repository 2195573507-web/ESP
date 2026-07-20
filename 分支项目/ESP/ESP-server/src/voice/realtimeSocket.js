const crypto = require("crypto");
const http = require("http");
const https = require("https");
const {
    extractErrorMessageFromBody,
    normalizeLogPreview
} = require("../utils/logging");
const {
    createVoiceStageError
} = require("./errors");

function createAbortError() {
    const error = new Error("Operation was aborted");
    error.name = "AbortError";
    return error;
}

function createWebSocketAcceptValue(key) {
    return crypto
        .createHash("sha1")
        .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)
        .digest("base64");
}

function encodeWebSocketFrame(opcode, payload = Buffer.alloc(0)) {
    const body = Buffer.isBuffer(payload) ? payload : Buffer.from(String(payload));
    const headerLength = body.length < 126 ? 2 : (body.length <= 0xffff ? 4 : 10);
    const maskKey = crypto.randomBytes(4);
    const frame = Buffer.alloc(headerLength + 4 + body.length);
    frame[0] = 0x80 | opcode;

    if (body.length < 126) {
        frame[1] = 0x80 | body.length;
        maskKey.copy(frame, 2);
        for (let i = 0; i < body.length; i += 1) {
            frame[6 + i] = body[i] ^ maskKey[i % 4];
        }
        return frame;
    }

    if (body.length <= 0xffff) {
        frame[1] = 0x80 | 126;
        frame.writeUInt16BE(body.length, 2);
        maskKey.copy(frame, 4);
        for (let i = 0; i < body.length; i += 1) {
            frame[8 + i] = body[i] ^ maskKey[i % 4];
        }
        return frame;
    }

    frame[1] = 0x80 | 127;
    frame.writeBigUInt64BE(BigInt(body.length), 2);
    maskKey.copy(frame, 10);
    for (let i = 0; i < body.length; i += 1) {
        frame[14 + i] = body[i] ^ maskKey[i % 4];
    }
    return frame;
}

function readReceiveLimit(value) {
    return Number.isSafeInteger(value) && value > 0 ? value : Number.MAX_SAFE_INTEGER;
}

class MinimalWebSocket {
    constructor(socket, stage, receiveLimits = {}) {
        this.socket = socket;
        this.stage = stage;
        this.buffer = Buffer.alloc(0);
        this.messages = [];
        this.queuedMessageBytes = 0;
        this.waiters = [];
        this.closed = false;
        this.closeError = null;
        this.closeSent = false;
        this.fragmentOpcode = 0;
        this.fragmentBuffers = [];
        this.fragmentBytes = 0;
        this.maxMessageBytes = readReceiveLimit(receiveLimits.maxMessageBytes);
        this.maxBufferedBytes = readReceiveLimit(receiveLimits.maxBufferedBytes);
        this.maxBufferedMessages = readReceiveLimit(receiveLimits.maxBufferedMessages);

        socket.on("data", chunk => this.onData(chunk));
        socket.on("error", error => this.fail(error));
        socket.on("close", () => this.close());
    }

    sendText(text) {
        if (this.closed) {
            throw createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_FAILED`, "Realtime WebSocket is closed", 502);
        }

        this.socket.write(encodeWebSocketFrame(0x1, Buffer.from(text)));
    }

    sendClose() {
        if (!this.closed && !this.closeSent) {
            this.closeSent = true;
            this.socket.end(encodeWebSocketFrame(0x8));
        }
    }

    abort(error) {
        if (this.closed) {
            return false;
        }

        this.closeError = error?.code
            ? error
            : createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_FAILED`, error?.message || "Realtime WebSocket aborted", 502, {
                cause: error
            });
        this.closed = true;
        this.messages = [];
        this.queuedMessageBytes = 0;
        this.buffer = Buffer.alloc(0);
        this.fragmentOpcode = 0;
        this.fragmentBuffers = [];
        this.fragmentBytes = 0;
        for (const waiter of this.waiters.splice(0)) {
            waiter.reject(this.closeError);
        }
        if (!this.socket.destroyed && typeof this.socket.destroy === "function") {
            this.socket.destroy();
        }
        return true;
    }

    close() {
        if (this.closed) {
            return;
        }

        this.closed = true;
        for (const waiter of this.waiters.splice(0)) {
            waiter.reject(this.closeError || createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_FAILED`, "Realtime WebSocket closed before completion", 502));
        }
    }

    fail(error) {
        this.closeError = error?.code
            ? error
            : createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_FAILED`, error?.message || "Realtime WebSocket failed", 502, {
                cause: error
            });
        this.close();
    }

    pushMessage(message) {
        if (this.closed) {
            return;
        }

        const messageBytes = Buffer.byteLength(message);
        if (messageBytes > this.maxMessageBytes ||
            this.messages.length >= this.maxBufferedMessages ||
            this.queuedMessageBytes + messageBytes > this.maxBufferedBytes) {
            this.abort(createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_BACKLOG_OVERFLOW`, "Realtime WebSocket receive backlog exceeded its limit", 502, {
                bytes: messageBytes
            }));
            return;
        }

        const waiter = this.waiters.shift();
        if (waiter) {
            waiter.resolve(message);
            return;
        }

        this.messages.push(message);
        this.queuedMessageBytes += messageBytes;
    }

    onData(chunk) {
        if (this.closed) {
            return;
        }
        this.buffer = Buffer.concat([this.buffer, chunk]);
        if (this.buffer.length > this.maxBufferedBytes) {
            this.abort(createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_BACKLOG_OVERFLOW`, "Realtime WebSocket frame buffer exceeded its limit", 502, {
                bytes: this.buffer.length
            }));
            return;
        }

        while (this.buffer.length >= 2) {
            const first = this.buffer[0];
            const second = this.buffer[1];
            const fin = (first & 0x80) !== 0;
            const opcode = first & 0x0f;
            const masked = (second & 0x80) !== 0;
            let payloadLength = second & 0x7f;
            let offset = 2;

            if (payloadLength === 126) {
                if (this.buffer.length < offset + 2) {
                    return;
                }
                payloadLength = this.buffer.readUInt16BE(offset);
                offset += 2;
            } else if (payloadLength === 127) {
                if (this.buffer.length < offset + 8) {
                    return;
                }
                const bigLength = this.buffer.readBigUInt64BE(offset);
                if (bigLength > BigInt(Number.MAX_SAFE_INTEGER)) {
                    this.fail(createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_FAILED`, "Realtime WebSocket frame is too large", 502));
                    return;
                }
                payloadLength = Number(bigLength);
                offset += 8;
            }

            if (payloadLength > this.maxMessageBytes) {
                this.abort(createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_BACKLOG_OVERFLOW`, "Realtime WebSocket frame exceeded its limit", 502, {
                    bytes: payloadLength
                }));
                return;
            }

            let maskKey = null;
            if (masked) {
                if (this.buffer.length < offset + 4) {
                    return;
                }
                maskKey = this.buffer.subarray(offset, offset + 4);
                offset += 4;
            }

            if (this.buffer.length < offset + payloadLength) {
                return;
            }

            let payload = this.buffer.subarray(offset, offset + payloadLength);
            this.buffer = this.buffer.subarray(offset + payloadLength);

            if (masked && maskKey) {
                const unmasked = Buffer.alloc(payload.length);
                for (let i = 0; i < payload.length; i += 1) {
                    unmasked[i] = payload[i] ^ maskKey[i % 4];
                }
                payload = unmasked;
            }

            this.handleFrame(opcode, fin, payload);
        }
    }

    handleFrame(opcode, fin, payload) {
        if (this.closed) {
            return;
        }
        if (opcode === 0x8) {
            this.sendClose();
            this.close();
            return;
        }

        if (opcode === 0x9) {
            this.socket.write(encodeWebSocketFrame(0xA, payload));
            return;
        }

        if (opcode === 0xA) {
            return;
        }

        if (opcode === 0x1 || opcode === 0x2) {
            if (fin) {
                this.pushMessage(payload.toString("utf8"));
                return;
            }
            this.fragmentOpcode = opcode;
            this.fragmentBuffers = [payload];
            this.fragmentBytes = payload.length;
            return;
        }

        if (opcode === 0x0 && this.fragmentOpcode !== 0) {
            this.fragmentBuffers.push(payload);
            this.fragmentBytes += payload.length;
            if (this.fragmentBytes > this.maxMessageBytes) {
                this.abort(createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_BACKLOG_OVERFLOW`, "Realtime WebSocket fragmented message exceeded its limit", 502, {
                    bytes: this.fragmentBytes
                }));
                return;
            }
            if (fin) {
                const message = Buffer.concat(this.fragmentBuffers).toString("utf8");
                this.fragmentOpcode = 0;
                this.fragmentBuffers = [];
                this.fragmentBytes = 0;
                this.pushMessage(message);
            }
            return;
        }

        this.fail(createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_FAILED`, `Unsupported Realtime WebSocket opcode ${opcode}`, 502));
    }

    nextMessage(signal) {
        if (this.messages.length > 0) {
            const message = this.messages.shift();
            this.queuedMessageBytes -= Buffer.byteLength(message);
            return Promise.resolve(message);
        }

        if (this.closed) {
            return Promise.reject(this.closeError || createVoiceStageError(this.stage, `VOICE_${this.stage.toUpperCase()}_FAILED`, "Realtime WebSocket closed before completion", 502));
        }

        return new Promise((resolve, reject) => {
            const waiter = { resolve, reject };
            const onAbort = () => {
                const index = this.waiters.indexOf(waiter);
                if (index >= 0) {
                    this.waiters.splice(index, 1);
                }
                reject(createAbortError());
            };

            if (signal?.aborted) {
                reject(createAbortError());
                return;
            }

            if (signal) {
                signal.addEventListener("abort", onAbort, { once: true });
            }

            waiter.resolve = value => {
                if (signal) {
                    signal.removeEventListener("abort", onAbort);
                }
                resolve(value);
            };
            waiter.reject = error => {
                if (signal) {
                    signal.removeEventListener("abort", onAbort);
                }
                reject(error);
            };
            this.waiters.push(waiter);
        });
    }
}

function openRealtimeWebSocket(url, headers, signal, stage, options = {}) {
    return new Promise((resolve, reject) => {
        let settled = false;
        let request = null;
        const parsed = new URL(url);
        const isSecure = parsed.protocol === "wss:";
        const transport = isSecure ? https : http;
        const key = crypto.randomBytes(16).toString("base64");
        const requestHeaders = {
            Host: parsed.host,
            Upgrade: "websocket",
            Connection: "Upgrade",
            "Sec-WebSocket-Key": key,
            "Sec-WebSocket-Version": "13",
            ...headers
        };

        const finishReject = error => {
            if (settled) {
                return;
            }
            settled = true;
            if (signal) {
                signal.removeEventListener("abort", onAbort);
            }
            reject(error);
        };
        const finishResolve = ws => {
            if (settled) {
                ws.sendClose();
                return;
            }
            settled = true;
            if (signal) {
                signal.removeEventListener("abort", onAbort);
            }
            resolve(ws);
        };
        const onAbort = () => {
            if (request) {
                request.destroy(createAbortError());
            }
            finishReject(createAbortError());
        };

        if (signal?.aborted) {
            finishReject(createAbortError());
            return;
        }
        if (signal) {
            signal.addEventListener("abort", onAbort, { once: true });
        }

        request = transport.request({
            protocol: isSecure ? "https:" : "http:",
            hostname: parsed.hostname,
            port: parsed.port || (isSecure ? 443 : 80),
            path: `${parsed.pathname}${parsed.search}`,
            method: "GET",
            headers: requestHeaders
        });

        request.once("upgrade", (response, socket, head) => {
            const expectedAccept = createWebSocketAcceptValue(key);
            const actualAccept = response.headers["sec-websocket-accept"];
            if (response.statusCode !== 101 || actualAccept !== expectedAccept) {
                socket.destroy();
                finishReject(createVoiceStageError(stage, `VOICE_${stage.toUpperCase()}_FAILED`, "Realtime WebSocket upgrade was rejected", 502, {
                    upstreamStatus: response.statusCode,
                    endpoint: url
                }));
                return;
            }

            const ws = new MinimalWebSocket(socket, stage, options.receiveLimits);
            if (head && head.length > 0) {
                ws.onData(head);
            }
            finishResolve(ws);
        });

        request.once("response", response => {
            const chunks = [];
            response.on("data", chunk => chunks.push(chunk));
            response.on("end", () => {
                const body = Buffer.concat(chunks).toString("utf8");
                finishReject(createVoiceStageError(stage, `VOICE_${stage.toUpperCase()}_FAILED`, extractErrorMessageFromBody(body) || "Realtime WebSocket upgrade failed", 502, {
                    upstreamStatus: response.statusCode,
                    bodyLength: body.length,
                    bodyPreview: normalizeLogPreview(body),
                    endpoint: url
                }));
            });
        });

        request.once("error", error => {
            if (error?.name === "AbortError") {
                finishReject(error);
                return;
            }
            finishReject(createVoiceStageError(stage, `VOICE_${stage.toUpperCase()}_FAILED`, error?.message || "Realtime WebSocket connection failed", 502, {
                endpoint: url,
                cause: error
            }));
        });

        request.end();
    });
}

module.exports = {
    MinimalWebSocket,
    createAbortError,
    encodeWebSocketFrame,
    openRealtimeWebSocket
};
