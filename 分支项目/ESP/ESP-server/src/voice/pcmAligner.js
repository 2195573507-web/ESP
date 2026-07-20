const {
    createVoiceStageError
} = require("./errors");

const STREAM_FORMAT_PROBE_BYTES = 12;

function rejectUnsupportedStreamingPrefix(prefix) {
    const startsWith = value => prefix.length >= value.length &&
        prefix.subarray(0, value.length).toString("ascii") === value;
    const startsWithBytes = values => prefix.length >= values.length &&
        values.every((value, index) => prefix[index] === value);
    const hasWavHeader = prefix.length >= 12 &&
        prefix.subarray(0, 4).toString("ascii") === "RIFF" &&
        prefix.subarray(8, 12).toString("ascii") === "WAVE";
    const hasMp3Sync = prefix.length >= 2 && prefix[0] === 0xff && (prefix[1] & 0xe0) === 0xe0;
    const hasMp4Header = prefix.length >= 8 && prefix.subarray(4, 8).toString("ascii") === "ftyp";
    const textPrefix = prefix.subarray(0, Math.min(prefix.length, 16)).toString("utf8").trimStart();
    const hasJsonPrefix = textPrefix.startsWith('{"') ||
        textPrefix.startsWith("[{") ||
        textPrefix.startsWith('["');

    if (!hasWavHeader &&
        !startsWith("ID3") &&
        !startsWith("OggS") &&
        !startsWith("fLaC") &&
        !startsWithBytes([0x1a, 0x45, 0xdf, 0xa3]) &&
        !hasMp3Sync &&
        !hasMp4Header &&
        !hasJsonPrefix) {
        return;
    }

    throw createVoiceStageError(
        "tts",
        "VOICE_TTS_UNSUPPORTED_FORMAT",
        "Realtime TTS streaming requires raw PCM s16le audio, not a container or encoded format",
        502,
        { bodyLength: prefix.length }
    );
}

class Pcm16Aligner {
    constructor() {
        this.carry = null;
    }

    push(chunk) {
        if (!Buffer.isBuffer(chunk)) {
            throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS PCM chunk must be a Buffer", 502);
        }
        if (chunk.length === 0) {
            return null;
        }

        const input = this.carry === null
            ? chunk
            : Buffer.concat([Buffer.from([this.carry]), chunk]);
        const alignedLength = input.length - (input.length % 2);
        this.carry = alignedLength === input.length ? null : input[input.length - 1];

        return alignedLength === 0 ? null : input.subarray(0, alignedLength);
    }

    finish() {
        if (this.carry === null) {
            return;
        }

        this.carry = null;
        throw createVoiceStageError("tts", "VOICE_TTS_ODD_LENGTH", "TTS PCM stream ended with an incomplete sample", 502);
    }

    reset() {
        this.carry = null;
    }
}

class RawPcm16StreamGate {
    constructor() {
        this.prefix = Buffer.alloc(0);
        this.checked = false;
    }

    push(chunk) {
        if (!Buffer.isBuffer(chunk)) {
            throw createVoiceStageError("tts", "VOICE_TTS_FAILED", "TTS PCM chunk must be a Buffer", 502);
        }
        if (chunk.length === 0) {
            return null;
        }
        if (this.checked) {
            return chunk;
        }

        this.prefix = Buffer.concat([this.prefix, chunk]);
        if (this.prefix.length < STREAM_FORMAT_PROBE_BYTES) {
            return null;
        }

        rejectUnsupportedStreamingPrefix(this.prefix);
        this.checked = true;
        const output = this.prefix;
        this.prefix = Buffer.alloc(0);
        return output;
    }

    finish() {
        if (this.checked || this.prefix.length === 0) {
            return null;
        }

        rejectUnsupportedStreamingPrefix(this.prefix);
        this.checked = true;
        const output = this.prefix;
        this.prefix = Buffer.alloc(0);
        return output;
    }

    reset() {
        this.prefix = Buffer.alloc(0);
        this.checked = false;
    }
}

module.exports = {
    Pcm16Aligner,
    RawPcm16StreamGate
};
