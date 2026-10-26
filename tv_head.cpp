#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ============================================================
// Microphone
// ============================================================

constexpr int MIC_PIN = 34;

// 約8 kHzでADCサンプリング
constexpr uint32_t MIC_SAMPLE_INTERVAL_US = 125;

// Peak-to-Peakを計算する時間窓
constexpr uint32_t AUDIO_WINDOW_US = 10000; // 10 ms

uint32_t lastMicSampleUs = 0;
uint32_t audioWindowStartUs = 0;

int audioMin = 4095;
int audioMax = 0;

float audioLevel = 0.0f;


// ============================================================
// Audio calibration
// ============================================================

// 実際にはSerial出力を見て調整してください。
constexpr float NOISE_FLOOR = 50.0f;
constexpr float LOUD_LEVEL   = 1200.0f;

// Attack:
// 大きな声へは速く追従
constexpr float ATTACK = 0.55f;

// Release:
// 声が止まった後は少しゆっくり戻る
constexpr float RELEASE = 0.08f;


// ============================================================
// Display
// ============================================================

constexpr int MATRIX_WIDTH  = 64;
constexpr int MATRIX_HEIGHT = 32;

constexpr uint32_t FRAME_INTERVAL_MS = 33; // 約30 FPS

uint32_t lastFrameMs = 0;


// ============================================================
// Eye geometry
// ============================================================

// 添付画像に近い配置
constexpr int EYE_TOP    = 0;
constexpr int EYE_BOTTOM = 31;

// 左目
constexpr int LEFT_EYE_L = 3;
constexpr int LEFT_EYE_R = 17;

// 右目
constexpr int RIGHT_EYE_L = 46;
constexpr int RIGHT_EYE_R = 60;


// ============================================================
// Noise parameters
// ============================================================

// 静音時にも常に少し揺らす
constexpr float BASE_NOISE = 0.7f;

// 最大発声時の追加揺れ
constexpr float MAX_AUDIO_NOISE = 5.5f;

// ランダム形状の更新速度
constexpr float SHAPE_SMOOTH = 0.35f;


// ============================================================
// Distortion state
// ============================================================

// 各Y座標に対して左右の輪郭を独立保持
float leftOuter[MATRIX_HEIGHT]  = {};
float leftInner[MATRIX_HEIGHT]  = {};
float rightInner[MATRIX_HEIGHT] = {};
float rightOuter[MATRIX_HEIGHT] = {};

float targetLeftOuter[MATRIX_HEIGHT]  = {};
float targetLeftInner[MATRIX_HEIGHT]  = {};
float targetRightInner[MATRIX_HEIGHT] = {};
float targetRightOuter[MATRIX_HEIGHT] = {};


// ============================================================
// Utility
// ============================================================

float randomFloat(float minValue, float maxValue)
{
    return minValue +
           (maxValue - minValue) *
           ((float)random(0, 10000) / 9999.0f);
}


// ============================================================
// Microphone processing
// ============================================================

void updateMicrophone()
{
    const uint32_t now = micros();

    // サンプリング周期になっていなければ何もしない
    if ((uint32_t)(now - lastMicSampleUs) < MIC_SAMPLE_INTERVAL_US)
    {
        return;
    }

    lastMicSampleUs = now;

    const int sample = analogRead(MIC_PIN);

    if (sample < audioMin)
    {
        audioMin = sample;
    }

    if (sample > audioMax)
    {
        audioMax = sample;
    }

    // 10msごとにPeak-to-Peakを確定
    if ((uint32_t)(now - audioWindowStartUs) >= AUDIO_WINDOW_US)
    {
        const float peakToPeak = (float)(audioMax - audioMin);

        // ノイズフロアを引いて0～1へ正規化
        float normalized =
            (peakToPeak - NOISE_FLOOR) /
            (LOUD_LEVEL - NOISE_FLOOR);

        normalized = constrain(normalized, 0.0f, 1.0f);

        // Attack / Release
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

        // 次の窓へ
        audioMin = 4095;
        audioMax = 0;
        audioWindowStartUs = now;
    }
}


// ============================================================
// Generate noise targets
// ============================================================

void generateXOffsetTargets(float amplitude)
{
    for (int y = 0; y < MATRIX_HEIGHT; y++)
    {
        targetLeftOuter[y] =
            randomFloat(-amplitude, amplitude);

        targetLeftInner[y] =
            randomFloat(-amplitude, amplitude);

        targetRightInner[y] =
            randomFloat(-amplitude, amplitude);

        targetRightOuter[y] =
            randomFloat(-amplitude, amplitude);
    }
}


// ============================================================
// Smooth noise shape
// ============================================================

void updateNoiseShape()
{
    for (int y = 0; y < MATRIX_HEIGHT; y++)
    {
        leftOuter[y] +=
            (targetLeftOuter[y] - leftOuter[y]) *
            SHAPE_SMOOTH;

        leftInner[y] +=
            (targetLeftInner[y] - leftInner[y]) *
            SHAPE_SMOOTH;

        rightInner[y] +=
            (targetRightInner[y] - rightInner[y]) *
            SHAPE_SMOOTH;

        rightOuter[y] +=
            (targetRightOuter[y] - rightOuter[y]) *
            SHAPE_SMOOTH;
    }
}


// ============================================================
// Draw one eye row
// ============================================================

void drawEyeRow(
    int y,
    int baseLeft,
    int baseRight,
    float leftNoise,
    float rightNoise,
    uint16_t color
)
{
    int x1 = baseLeft + (int)roundf(leftNoise);
    int x2 = baseRight + (int)roundf(rightNoise);

    x1 = constrain(x1, 0, MATRIX_WIDTH - 1);
    x2 = constrain(x2, 0, MATRIX_WIDTH - 1);

    // 左右が逆転しないように保証
    if (x1 > x2)
    {
        const int tmp = x1;
        x1 = x2;
        x2 = tmp;
    }

    dma_display->drawFastHLine(
        x1,
        y,
        x2 - x1 + 1,
        color
    );
}


// ============================================================
// Render
// ============================================================

void renderEyes()
{
    const uint32_t now = millis();

    if ((uint32_t)(now - lastFrameMs) < FRAME_INTERVAL_MS)
    {
        return;
    }

    lastFrameMs = now;

    /*
       音量 → ノイズ振幅

       silent:
       0.7px程度

       loud:
       約6.2px
    */
    const float amplitude =
        BASE_NOISE +
        audioLevel * MAX_AUDIO_NOISE;

    generateXOffsetTargets(amplitude);

    updateNoiseShape();

    const uint16_t white =
        dma_display->color565(255, 255, 255);

    dma_display->clearScreen();

    for (int y = EYE_TOP; y <= EYE_BOTTOM; y++)
    {
        // 左目
        drawEyeRow(
            y,
            LEFT_EYE_L,
            LEFT_EYE_R,
            leftOuter[y],
            leftInner[y],
            white
        );

        // 右目
        drawEyeRow(
            y,
            RIGHT_EYE_L,
            RIGHT_EYE_R,
            rightInner[y],
            rightOuter[y],
            white
        );
    }
}


// ============================================================
// Setup microphone
// ============================================================

void setupMicrophone()
{
    pinMode(MIC_PIN, INPUT);

    analogReadResolution(12);

    audioWindowStartUs = micros();
    lastMicSampleUs = micros();

    randomSeed(
        analogRead(MIC_PIN) ^
        micros()
    );
}


// ============================================================
// Main processing
// ============================================================

// 既存のsetup()から呼ぶ
void setupAudioEyes()
{
    setupMicrophone();
}

// 既存のloop()から毎回呼ぶ
void updateAudioEyes()
{
    // マイク処理は可能な限り頻繁に呼ぶ
    updateMicrophone();

    // LED描画は約30FPS
    renderEyes();
}