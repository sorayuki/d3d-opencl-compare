#ifndef AUTOSCOPIA_COLORCONV_COLOR_TYPES_H_
#define AUTOSCOPIA_COLORCONV_COLOR_TYPES_H_


namespace asco {

    /**
     * 描述了 RGB 色域
     */
    enum class ColorPrimaries {
        Unknown,
        Custom,
        BT709,
        BT470_2_SysM,
        BT470_2_SysBG,
        SMPTE170M,
        SMPTE240M,
        EBU3213,
        SMPTE_C,
        BT2020,
        XYZ,
        DCI_P3,
        ACES,
    };

    /**
     * 描述了从线性 RGB 到非线性 R'G'B' 的转换公式
     */
    enum class ColorTransFunc {
        Unknown,
        _10,
        _18,
        _20,
        _22,
        _709,
        _240M,
        sRGB,
        _28,
        Log_100,
        Log_316,
        _709_Sym,
        _2020_const,
        _2020,
        _26,
        _2084,   // SMPTE ST 2084 (PQ)
        HLG,     // ARIB STD-B67 (HLG)
        _10_rel,
    };

    /**
     * 描述了在 Y'PbPr 和 R'G'B' 之间转换的矩阵
     */
    enum class ColorTransMatrix {
        Unknown,
        BT709,
        BT601,
        SMPTE240M,
        BT2020_10,
        BT2020_12,
    };

    enum class ColorNominalRange {
        Unknown,
        _0_255,
        _16_235,
        _48_208,
        _64_127,
    };

    enum class ColorLighting {
        Unknown,
        Bright,
        Office,
        Dim,
        Dark,
    };

    struct RGBSystem {
        float rx;
        float ry;
        float gx;
        float gy;
        float bx;
        float by;
        float wx;
        float wy;
    };

    struct ColorInfo {
        ColorPrimaries primaries;
        ColorTransFunc trans_func;
        ColorTransMatrix trans_matrix;
        ColorNominalRange nominal_range;
        ColorLighting lighting;
        RGBSystem custom_primaries;
    };


}

#endif  // AUTOSCOPIA_COLORCONV_COLOR_TYPES_H_