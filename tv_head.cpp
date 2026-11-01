#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#define PANEL_WIDTH  64
#define PANEL_HEIGHT 32
#define PANEL_CHAIN  1

HUB75_I2S_CFG::i2s_pins pins = {
  25, 26, 27,       // R1, G1, B1
  14, 12, 13,       // R2, G2, B2
  23, 19, 5, 17, -1,// A, B, C, D, E
  4, 15, 16         // LAT, OE, CLK
};

HUB75_I2S_CFG config(
  PANEL_WIDTH,
  PANEL_HEIGHT,
  PANEL_CHAIN,
  pins
);

MatrixPanel_I2S_DMA display(config);


// ============================================================
// Matrix
// ============================================================

constexpr int MATRIX_W = 64;
constexpr int MATRIX_H = 32;


// ============================================================
// Microphone
// ============================================================

constexpr int MIC_PIN = 34;

// 約8kHz
constexpr uint32_t MIC_SAMPLE_INTERVAL_US = 125;

// 10ms単位でPeak-to-Peak計算
constexpr uint32_t AUDIO_WINDOW_US = 10000;

uint32_t lastMicSampleUs = 0;
uint32_t audioWindowStartUs = 0;

int micMin = 4095;
int micMax = 0;

float audioLevel = 0.0f;
float lastPeakToPeak = 0.0f;

// シリアル診断出力（感度調整用）
constexpr uint32_t SERIAL_DEBUG_INTERVAL_MS = 100;
uint32_t lastSerialDebugMs = 0;


// ============================================================
// Microphone calibration
// ============================================================

// 実機で調整
constexpr float NOISE_FLOOR = 45.0f;
constexpr float LOUD_LEVEL  = 1100.0f;

// 発声開始は速く反応
constexpr float ATTACK = 0.55f;

// 発声終了はゆっくり戻す
constexpr float RELEASE = 0.075f;


// ============================================================
// Rendering
// ============================================================

constexpr uint32_t FRAME_INTERVAL_MS = 33; // 約30fps

uint32_t lastFrameMs = 0;

// 音量に応じて追加するランダム横線
// 線の長さは 1～64 pixel の範囲で毎回ランダムに決める。
constexpr int MAX_AUDIO_LINES = 3;


// ============================================================
// Blink
// ============================================================

// 次の瞬きまでの待機時間
constexpr uint32_t BLINK_INTERVAL_MIN_MS = 2000;
constexpr uint32_t BLINK_INTERVAL_MAX_MS = 5000;

// 消灯・点灯にかける時間
constexpr uint32_t BLINK_CLOSE_DURATION_MS = 220;
constexpr uint32_t BLINK_OPEN_DURATION_MS  = 260;

enum BlinkState : uint8_t
{
    BLINK_IDLE,
    BLINK_CLOSING,
    BLINK_OPENING
};

BlinkState blinkState = BLINK_IDLE;
uint32_t nextBlinkMs = 0;
uint32_t blinkStateStartMs = 0;

bool isBlinkRowVisible(int y);


// ============================================================
// Eye geometry
// ============================================================

// 添付画像に近い形状
constexpr int LEFT_BASE_L  = 3;
constexpr int LEFT_BASE_R  = 17;

constexpr int RIGHT_BASE_L = 46;
constexpr int RIGHT_BASE_R = 60;


// ============================================================
// Smooth noise
// ============================================================

// ノイズグリッド
//
// 32行すべてに乱数を与えるのではなく、
// 少数の制御点を生成して補間する。
//
constexpr int NOISE_POINTS = 9;

float noiseA[NOISE_POINTS];
float noiseB[NOISE_POINTS];

float targetNoiseA[NOISE_POINTS];
float targetNoiseB[NOISE_POINTS];


// 時間方向の変化速度
constexpr float NOISE_TIME_SMOOTH = 0.12f;


// ============================================================
// Random helpers
// ============================================================

float randFloat(float minVal, float maxVal)
{
    return minVal +
           ((float)random(0, 10000) / 9999.0f) *
           (maxVal - minVal);
}


// ============================================================
// Smooth interpolation
// ============================================================

// Smoothstep
float smoothStep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}


// ============================================================
// Spatial smooth noise
// ============================================================

float sampleSmoothNoise(
    const float* points,
    float y
)
{
    // y = 0～31
    const float normalized =
        y / (float)(MATRIX_H - 1);

    const float pos =
        normalized * (NOISE_POINTS - 1);

    int index = (int)floorf(pos);

    if (index >= NOISE_POINTS - 1)
    {
        return points[NOISE_POINTS - 1];
    }

    const float localT = pos - index;

    const float t = smoothStep(localT);

    const float a = points[index];
    const float b = points[index + 1];

    return a + (b - a) * t;
}


// ============================================================
// Audio
// ============================================================

void updateMicrophone()
{
    const uint32_t now = micros();

    if ((uint32_t)(now - lastMicSampleUs)
        < MIC_SAMPLE_INTERVAL_US)
    {
        return;
    }

    lastMicSampleUs = now;

    const int sample = analogRead(MIC_PIN);

    if (sample < micMin)
        micMin = sample;

    if (sample > micMax)
        micMax = sample;

    if ((uint32_t)(now - audioWindowStartUs)
        >= AUDIO_WINDOW_US)
    {
        const float peakToPeak =
            (float)(micMax - micMin);

        lastPeakToPeak = peakToPeak;

        float normalized =
            (peakToPeak - NOISE_FLOOR) /
            (LOUD_LEVEL - NOISE_FLOOR);

        normalized =
            constrain(normalized, 0.0f, 1.0f);

        // 少し非線形にする
        // 小声では控えめ、大声になるほど急激に変化
        normalized = powf(normalized, 1.35f);

        if (normalized > audioLevel)
        {
            audioLevel +=
                (normalized - audioLevel) * ATTACK;
        }
        else
        {
            audioLevel +=
                (normalized - audioLevel) * RELEASE;
        }

        micMin = 4095;
        micMax = 0;

        audioWindowStartUs = now;
    }
}


// ============================================================
// Noise target generation
// ============================================================

void generateNoiseTargets()
{
    for (int i = 0; i < NOISE_POINTS; i++)
    {
        targetNoiseA[i] =
            randFloat(-1.0f, 1.0f);

        targetNoiseB[i] =
            randFloat(-1.0f, 1.0f);
    }
}


// ============================================================
// Temporal smoothing
// ============================================================

void updateNoiseField()
{
    for (int i = 0; i < NOISE_POINTS; i++)
    {
        noiseA[i] +=
            (targetNoiseA[i] - noiseA[i])
            * NOISE_TIME_SMOOTH;

        noiseB[i] +=
            (targetNoiseB[i] - noiseB[i])
            * NOISE_TIME_SMOOTH;
    }
}


// ============================================================
// High-frequency glitch
// ============================================================

float getFineGlitch(
    int y,
    float audio
)
{
    // 発声していない時はほぼ0
    if (audio < 0.03f)
    {
        return 0.0f;
    }

    // 音量が大きいほどグリッチが起きやすくなる
    const float probability =
        0.03f + audio * 0.28f;

    const float r =
        (float)random(0, 10000) / 10000.0f;

    if (r > probability)
    {
        return 0.0f;
    }

    // グリッチ幅
    const float maxOffset =
        1.0f + audio * 5.0f;

    return randFloat(
        -maxOffset,
        maxOffset
    );
}


// ============================================================
// Draw eye row
// ============================================================

void drawEyeRow(
    int y,
    int baseLeft,
    int baseRight,
    float leftOffset,
    float rightOffset,
    uint16_t color
)
{
    int x1 =
        baseLeft + (int)roundf(leftOffset);

    int x2 =
        baseRight + (int)roundf(rightOffset);

    x1 = constrain(x1, 0, MATRIX_W - 1);
    x2 = constrain(x2, 0, MATRIX_W - 1);

    // 最低幅確保
    constexpr int MIN_WIDTH = 5;

    if ((x2 - x1) < MIN_WIDTH)
    {
        const int center = (x1 + x2) / 2;

        x1 = center - MIN_WIDTH / 2;
        x2 = center + MIN_WIDTH / 2;

        x1 = constrain(x1, 0, MATRIX_W - 1);
        x2 = constrain(x2, 0, MATRIX_W - 1);
    }

    if (x1 > x2)
    {
        int temp = x1;
        x1 = x2;
        x2 = temp;
    }

    display.drawFastHLine(
        x1,
        y,
        x2 - x1 + 1,
        color
    );
}


// ============================================================
// Audio random lines
// ============================================================

void drawAudioRandomLines(uint16_t color)
{
    // 音量が大きいほど、1フレームに描く線の本数を増やす。
    // 小数部分は確率で繰り上げ、音量変化を滑らかに反映する。
    const float expectedLines =
        audioLevel * MAX_AUDIO_LINES;

    int lineCount = (int)expectedLines;

    const float fractionalPart =
        expectedLines - lineCount;

    if (randFloat(0.0f, 1.0f) < fractionalPart)
    {
        lineCount++;
    }

    for (int i = 0; i < lineCount; i++)
    {
        // 長さは 1～64 pixel。開始位置を調整することで、
        // 横幅を超えずに最大64 pixelの線を描く。
        const int length =
            random(1, MATRIX_W + 1);

        const int x =
            random(0, MATRIX_W - length + 1);

        const int y =
            random(0, MATRIX_H);

        // 瞬きで消灯中の行には、横線も描画しない。
        if (!isBlinkRowVisible(y))
        {
            continue;
        }

        display.drawFastHLine(
            x,
            y,
            length,
            color
        );
    }
}


// ============================================================
// Blink animation
// ============================================================

uint32_t randomBlinkInterval()
{
    return (uint32_t)random(
        (long)BLINK_INTERVAL_MIN_MS,
        (long)BLINK_INTERVAL_MAX_MS + 1L
    );
}


void updateBlink()
{
    const uint32_t now = millis();

    if (blinkState == BLINK_IDLE)
    {
        if ((int32_t)(now - nextBlinkMs) >= 0)
        {
            blinkState = BLINK_CLOSING;
            blinkStateStartMs = now;
        }

        return;
    }

    const uint32_t duration =
        blinkState == BLINK_CLOSING
            ? BLINK_CLOSE_DURATION_MS
            : BLINK_OPEN_DURATION_MS;

    if ((uint32_t)(now - blinkStateStartMs) < duration)
    {
        return;
    }

    if (blinkState == BLINK_CLOSING)
    {
        blinkState = BLINK_OPENING;
        blinkStateStartMs = now;
    }
    else
    {
        blinkState = BLINK_IDLE;
        nextBlinkMs = now + randomBlinkInterval();
    }
}


bool isBlinkRowVisible(int y)
{
    if (blinkState == BLINK_IDLE)
    {
        return true;
    }

    const uint32_t duration =
        blinkState == BLINK_CLOSING
            ? BLINK_CLOSE_DURATION_MS
            : BLINK_OPEN_DURATION_MS;

    const float progress = constrain(
        (float)(millis() - blinkStateStartMs) /
            (float)duration,
        0.0f,
        1.0f
    );

    if (blinkState == BLINK_CLOSING)
    {
        // 消灯時は上から下へ表示境界を移動させる。
        const float cutoff =
            progress * MATRIX_H;

        return (float)y >= cutoff;
    }

    // 点灯時は下から上へ表示境界を移動させる。
    const float cutoff =
        (1.0f - progress) * MATRIX_H;

    return (float)y >= cutoff;
}


// ============================================================
// Render
// ============================================================

void renderGlitchEyes()
{
    const uint32_t now = millis();

    if ((uint32_t)(now - lastFrameMs)
        < FRAME_INTERVAL_MS)
    {
        return;
    }

    lastFrameMs = now;

    // --------------------------------------------------------
    // Noise target更新
    //
    // 毎フレーム完全に乱数を変えるのではなく、
    // 約5フレームごとに新しい目標を作る。
    // --------------------------------------------------------

    static int targetCounter = 0;

    targetCounter++;

    if (targetCounter >= 5)
    {
        generateNoiseTargets();
        targetCounter = 0;
    }

    updateNoiseField();


    // --------------------------------------------------------
    // Audio → distortion
    // --------------------------------------------------------

    constexpr float BASE_AMPLITUDE = 0.9f;
    constexpr float AUDIO_AMPLITUDE = 9.5f;

    const float amplitude =
        BASE_AMPLITUDE +
        audioLevel * AUDIO_AMPLITUDE;


    // --------------------------------------------------------
    // Color
    // --------------------------------------------------------

    const uint16_t white =
        display.color565(
            255,
            255,
            255
        );

    const uint16_t miku =
        display.color565(
            74,
            255,
            253
        );

    const uint16_t miku_pink =
        display.color565(
            225,
            100,
            180
        ); 

    // --------------------------------------------------------
    // Clear
    // --------------------------------------------------------

    display.clearScreen();


    // --------------------------------------------------------
    // Render rows
    // --------------------------------------------------------

    for (int y = 0; y < MATRIX_H; y++)
    {
        // 目も横線も、瞬きの表示境界に合わせて行単位で描画する。
        if (!isBlinkRowVisible(y))
        {
            continue;
        }

        // 大きく滑らかな揺れ
        const float smoothA =
            sampleSmoothNoise(
                noiseA,
                (float)y
            );

        const float smoothB =
            sampleSmoothNoise(
                noiseB,
                (float)y
            );


        // --------------------------------------------
        // 小さい波を追加
        // --------------------------------------------

        const float phase =
            millis() * 0.0035f;

        const float wave1 =
            sinf(
                y * 0.42f +
                phase
            );

        const float wave2 =
            sinf(
                y * 0.73f -
                phase * 1.7f
            );


        // --------------------------------------------
        // 基本変位
        // --------------------------------------------

        float leftNoise =
            smoothA * amplitude
            +
            wave1 * 0.55f;

        float rightNoise =
            smoothB * amplitude
            +
            wave2 * 0.55f;


        // --------------------------------------------
        // 発声時だけ細かなglitch追加
        // --------------------------------------------

        const float glitchL =
            getFineGlitch(
                y,
                audioLevel
            );

        const float glitchR =
            getFineGlitch(
                y,
                audioLevel
            );

        leftNoise += glitchL;
        rightNoise += glitchR;


        // --------------------------------------------
        // 左目
        //
        // 左右を少し違うノイズにすることで
        // 単純な平行移動ではなく変形にする
        // --------------------------------------------

        drawEyeRow(
            y,

            LEFT_BASE_L,
            LEFT_BASE_R,

            leftNoise,

            rightNoise * 0.75f,

            white
        );


        // ---------------------------------------  -----
        // 右目
        //
        // 左目とは少し位相・方向をずらす
        // --------------------------------------------

        drawEyeRow(
            y,

            RIGHT_BASE_L,
            RIGHT_BASE_R,

            -rightNoise * 0.7f,

            -leftNoise,

            white
        );
    }

    // 目の描画後に重ね、音量に応じたテレビ風の走査ノイズにする。
    drawAudioRandomLines(white);
}


// ============================================================
// Init
// ============================================================

void setupGlitchEyes()
{
    pinMode(
        MIC_PIN,
        INPUT
    );

    analogReadResolution(12);

    audioWindowStartUs =
        micros();

    lastMicSampleUs =
        micros();

    randomSeed(
        analogRead(MIC_PIN)
        ^
        micros()
    );

    nextBlinkMs =
        millis() + randomBlinkInterval();

    // 初期ノイズ
    for (int i = 0; i < NOISE_POINTS; i++)
    {
        noiseA[i] =
            randFloat(-1.0f, 1.0f);

        noiseB[i] =
            randFloat(-1.0f, 1.0f);

        targetNoiseA[i] =
            noiseA[i];

        targetNoiseB[i] =
            noiseB[i];
    }
}


// ============================================================
// Serial diagnostics
// ============================================================

/*
void updateSerialDebug()
{
    const uint32_t now = millis();

    if ((uint32_t)(now - lastSerialDebugMs) < SERIAL_DEBUG_INTERVAL_MS)
    {
        return;
    }

    lastSerialDebugMs = now;

    Serial.print("P2P:");
    Serial.print(lastPeakToPeak, 0);
    Serial.print("  level:");
    Serial.println(audioLevel, 3);
}
*/


// ============================================================
// Main update
// ============================================================

void updateGlitchEyes()
{
    updateMicrophone();
    updateBlink();
    renderGlitchEyes();
    // updateSerialDebug();
}

void setup()
{
    // Serial.begin(115200);

    // HUB75初期化
    display.begin();
    display.clearScreen();

    setupGlitchEyes();
}

void loop()
{
    updateGlitchEyes();
}
