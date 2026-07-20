const {
    VOICE_TURN_SAMPLE_RATE
} = require("./http");

const VOICE_TURN_MOCK_BEEP_HZ = 1000;
const VOICE_TURN_MOCK_BEEP_AMPLITUDE = 12000;
const VOICE_PROMPT_MOCK_BYTES = VOICE_TURN_SAMPLE_RATE * 2;

function createMockVoiceTurnPcm(byteLength) {
    const output = Buffer.alloc(byteLength);
    const sampleCount = Math.floor(byteLength / 2);

    for (let i = 0; i < sampleCount; i += 1) {
        const phase = (2 * Math.PI * VOICE_TURN_MOCK_BEEP_HZ * i) / VOICE_TURN_SAMPLE_RATE;
        const sample = Math.round(Math.sin(phase) * VOICE_TURN_MOCK_BEEP_AMPLITUDE);
        output.writeInt16LE(sample, i * 2);
    }

    return output;
}

async function streamMockVoiceTurn(audioBuffer) {
    const pcm = createMockVoiceTurnPcm(audioBuffer.length);

    return {
        bytes: pcm.length,
        mode: "mock",
        asrTextLength: 0,
        llmReplyLength: 0,
        ttsPcmBytes: 0,
        pcm
    };
}

function createMockVoicePromptPcm() {
    return createMockVoiceTurnPcm(VOICE_PROMPT_MOCK_BYTES);
}

module.exports = {
    createMockVoicePromptPcm,
    streamMockVoiceTurn
};
