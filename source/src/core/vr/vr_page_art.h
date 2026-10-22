// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// GENERATED offline (by a tool not included here) from the game's menu/optionsetting.gfx (the
// Environment settings page, EnvironmentSetting #107 at its 'Loop' frame): every bitmap-filled
// rectangle the movie draws for the page frame and the row widgets, in stage px (1920 x 1080)
// relative to the part's origin, with the affine map to the atlas's texels
// (u = u0 + ux * x + uy * y, v = v0 + vx * x + vy * y). The pixels are the game's own, sampled at
// runtime from menu/common.tpf.dcx (MENU_Common_00003 / 00001, BC7); nothing of them is in here.
// Blend Add = Scaleform's additive blend (the glows), alpha = the movie's colour-transform alpha.
// Do not edit by hand.

#pragma once

#include <array>

namespace VR::PageArt {

enum class Atlas : unsigned char { Common3, Common1, Solid };
enum class Blend : unsigned char { Over, Add };

struct Piece {
    Atlas atlas;
    Blend blend;
    bool smooth;
    bool repeat;
    float x0, y0, x1, y1;
    float ux, uy, vx, vy, u0, v0;
    float alpha;
};

// Part origins (stage px). Rows: the movie's Item_N_0 of the Environment page; the button row:
// Item_5_0 (its right-hand position; the left button is its mirror about PageCentreX).
inline constexpr float RowX = 594.300f;
inline constexpr std::array<float, 5> RowY{281.650f, 334.250f, 425.900f, 482.450f, 539.000f};
inline constexpr float ButtonX = 1042.300f;
inline constexpr float ButtonY = 682.450f;
// The slider's fill: the full width (stage px) the value's frame mask uncovers (frame = value).
inline constexpr float SliderFillWidth = 305.000f;
// The frame's extent (stage px): the page's panel (hit tests; the left button mirrors about its centre).
inline constexpr float FrameX0 = 417.500f;
inline constexpr float FrameY0 = 151.700f;
inline constexpr float FrameX1 = 1491.400f;
inline constexpr float FrameY1 = 921.350f;
// The first FrameBackdropCount pieces of Frame are the panel itself (#21: its black middle and
// soft 9-slice edges), where the page's own screen dim (#19, 60% black over everything behind
// the page in the game) is drawn under it.
inline constexpr int FrameBackdropCount = 9;
inline constexpr float ScreenDimAlpha = 0.6016f;
// The scratched panel texture's mirrored halves (Frame[9..12]: their inner edges leave a dark
// gutter down the middle) and the column divider over it (Frame[39..40], x 955..961): left out
// of a single-column page (View::single_column, the lantern's), whose centred rows they cut.
// (Positional in this generated table: re-check them if gen_page_art.py changes it.)
inline constexpr int FrameTextureFirst = 9;
inline constexpr int FrameTextureEnd = 13;
inline constexpr int FrameDividerFirst = 39;
inline constexpr int FrameDividerEnd = 41;

inline constexpr Piece Frame[] = {
    {Atlas::Common3, Blend::Over, false, true, 417.500f, 170.400f, 585.500f, 197.400f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -395.5000f, -50.2500f, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9
    {Atlas::Common3, Blend::Over, false, true, 417.500f, 197.350f, 585.500f, 894.350f, 1.000000f, 0.000000f, 0.000000f, 0.120517f, -395.5000f, 124.4097f, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9
    {Atlas::Common3, Blend::Over, false, true, 417.500f, 894.350f, 585.500f, 921.350f, 1.000000f, 0.000000f, 0.000000f, -1.000000f, -395.5000f, 1041.5000f, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9
    {Atlas::Common3, Blend::Over, false, true, 585.450f, 170.400f, 1323.450f, 197.400f, 0.023036f, 0.000000f, 0.000000f, 1.000000f, 159.7138f, -50.2500f, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9
    {Atlas::Solid, Blend::Over, false, false, 585.450f, 197.350f, 1323.450f, 894.350f, 0, 0, 0, 0, 0, 0, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9 (black)
    {Atlas::Common3, Blend::Over, false, true, 585.450f, 894.350f, 1323.450f, 921.350f, 0.023036f, 0.000000f, 0.000000f, -1.000000f, 159.7138f, 1041.5000f, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9
    {Atlas::Common3, Blend::Over, false, true, 1323.400f, 170.400f, 1491.400f, 197.400f, -1.000000f, 0.000000f, 0.000000f, 1.000000f, 1513.4000f, -50.2500f, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9
    {Atlas::Common3, Blend::Over, false, true, 1323.400f, 197.350f, 1491.400f, 894.350f, -1.000000f, 0.000000f, 0.000000f, 0.120517f, 1513.4000f, 124.4157f, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9
    {Atlas::Common3, Blend::Over, false, true, 1323.400f, 894.350f, 1491.400f, 921.350f, -1.000000f, 0.000000f, 0.000000f, -1.000000f, 1513.4000f, 1041.5000f, 0.6992f},  // /EnvironmentSetting/#34@3/#21@1/#20@9
    {Atlas::Common3, Blend::Add, false, true, 679.950f, 255.050f, 957.950f, 436.050f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -673.8000f, 57.1000f, 0.6992f},  // /EnvironmentSetting/#34@3/#23@11/#22@2
    {Atlas::Common3, Blend::Add, false, true, 679.950f, 436.050f, 957.950f, 614.450f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -673.8000f, 181.6500f, 0.6992f},  // /EnvironmentSetting/#34@3/#23@11/#22@2
    {Atlas::Common3, Blend::Add, false, true, 959.950f, 433.505f, 1237.950f, 614.450f, -1.000000f, 0.000000f, 0.000000f, -1.000305f, 1244.1000f, 926.7876f, 0.6992f},  // /EnvironmentSetting/#34@3/#23@14/#22@2
    {Atlas::Common3, Blend::Add, false, true, 959.950f, 255.160f, 1237.950f, 433.505f, -1.000000f, 0.000000f, 0.000000f, -1.000305f, 1244.1000f, 1051.3376f, 0.6992f},  // /EnvironmentSetting/#34@3/#23@14/#22@2
    {Atlas::Common3, Blend::Add, false, true, 533.750f, 254.700f, 563.750f, 260.700f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -522.5000f, -227.7500f, 0.3008f},  // /EnvironmentSetting/#34@3/#25@17/#24@7
    {Atlas::Common3, Blend::Add, false, true, 563.300f, 254.700f, 952.300f, 260.700f, 0.154237f, 0.000000f, 0.000000f, 1.000000f, -44.1273f, -227.7500f, 0.3008f},  // /EnvironmentSetting/#34@3/#25@17/#24@7
    {Atlas::Common3, Blend::Add, false, true, 1354.350f, 254.700f, 1384.350f, 260.700f, -1.000000f, 0.000000f, 0.000000f, 1.000000f, 1395.6000f, -227.7500f, 0.3008f},  // /EnvironmentSetting/#34@3/#25@17/#24@7
    {Atlas::Common3, Blend::Add, false, true, 965.450f, 254.700f, 1354.450f, 260.700f, 0.154237f, 0.000000f, 0.000000f, 1.000000f, -106.1538f, -227.7500f, 0.3008f},  // /EnvironmentSetting/#34@3/#25@17/#24@7
    {Atlas::Common3, Blend::Add, false, true, 949.150f, 234.700f, 968.150f, 269.700f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -566.7000f, -221.1500f, 0.3008f},  // /EnvironmentSetting/#34@3/#25@17/#24@7
    {Atlas::Common3, Blend::Add, false, true, 907.650f, 243.750f, 952.300f, 256.250f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -833.3500f, -170.8500f, 0.3008f},  // /EnvironmentSetting/#34@3/#25@17/#24@7
    {Atlas::Common3, Blend::Add, false, true, 965.550f, 243.750f, 1010.200f, 256.250f, -1.000000f, 0.000000f, 0.000000f, 1.000000f, 1084.5000f, -170.8500f, 0.3008f},  // /EnvironmentSetting/#34@3/#25@17/#24@7
    {Atlas::Common3, Blend::Add, false, true, 533.750f, 404.550f, 563.750f, 410.550f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -522.5000f, -377.6000f, 0.1992f},  // /EnvironmentSetting/#34@3/#27@25/#26@5
    {Atlas::Common3, Blend::Add, false, true, 563.300f, 404.550f, 952.300f, 410.550f, 0.154237f, 0.000000f, 0.000000f, 1.000000f, -44.1273f, -377.6000f, 0.1992f},  // /EnvironmentSetting/#34@3/#27@25/#26@5
    {Atlas::Common3, Blend::Add, false, true, 1354.350f, 404.550f, 1384.350f, 410.550f, -1.000000f, 0.000000f, 0.000000f, 1.000000f, 1395.6000f, -377.6000f, 0.1992f},  // /EnvironmentSetting/#34@3/#27@25/#26@5
    {Atlas::Common3, Blend::Add, false, true, 965.450f, 404.550f, 1354.450f, 410.550f, 0.154237f, 0.000000f, 0.000000f, 1.000000f, -106.1538f, -377.6000f, 0.1992f},  // /EnvironmentSetting/#34@3/#27@25/#26@5
    {Atlas::Common3, Blend::Add, false, true, 949.150f, 384.550f, 968.150f, 419.550f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -566.7000f, -371.0000f, 0.1992f},  // /EnvironmentSetting/#34@3/#27@25/#26@5
    {Atlas::Common3, Blend::Add, false, true, 533.750f, 614.500f, 563.750f, 620.500f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -522.5000f, -587.5500f, 0.1992f},  // /EnvironmentSetting/#34@3/#29@31/#28@7
    {Atlas::Common3, Blend::Add, false, true, 563.300f, 614.500f, 952.300f, 620.500f, 0.154237f, 0.000000f, 0.000000f, 1.000000f, -44.1273f, -587.5500f, 0.1992f},  // /EnvironmentSetting/#34@3/#29@31/#28@7
    {Atlas::Common3, Blend::Add, false, true, 1354.350f, 614.500f, 1384.350f, 620.500f, -1.000000f, 0.000000f, 0.000000f, 1.000000f, 1395.6000f, -587.5500f, 0.1992f},  // /EnvironmentSetting/#34@3/#29@31/#28@7
    {Atlas::Common3, Blend::Add, false, true, 965.450f, 614.500f, 1354.450f, 620.500f, 0.154237f, 0.000000f, 0.000000f, 1.000000f, -106.1538f, -587.5500f, 0.1992f},  // /EnvironmentSetting/#34@3/#29@31/#28@7
    {Atlas::Common3, Blend::Add, false, true, 949.150f, 594.500f, 968.150f, 621.650f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -566.7000f, -580.9500f, 0.1992f},  // /EnvironmentSetting/#34@3/#29@31/#28@7
    {Atlas::Common3, Blend::Add, false, true, 907.650f, 603.550f, 952.300f, 616.050f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -833.3500f, -530.6500f, 0.1992f},  // /EnvironmentSetting/#34@3/#29@31/#28@7
    {Atlas::Common3, Blend::Add, false, true, 965.550f, 603.550f, 1010.200f, 616.050f, -1.000000f, 0.000000f, 0.000000f, 1.000000f, 1084.5000f, -530.6500f, 0.1992f},  // /EnvironmentSetting/#34@3/#29@31/#28@7
    {Atlas::Common3, Blend::Add, false, true, 533.750f, 732.900f, 563.750f, 738.900f, 1.000000f, 0.000000f, 0.000000f, -1.000000f, -522.5000f, 765.8500f, 0.3008f},  // /EnvironmentSetting/#34@3/#29@39/#28@7
    {Atlas::Common3, Blend::Add, false, true, 563.300f, 732.900f, 952.300f, 738.900f, 0.154237f, 0.000000f, 0.000000f, -1.000000f, -44.1273f, 765.8500f, 0.3008f},  // /EnvironmentSetting/#34@3/#29@39/#28@7
    {Atlas::Common3, Blend::Add, false, true, 1354.350f, 732.900f, 1384.350f, 738.900f, -1.000000f, 0.000000f, 0.000000f, -1.000000f, 1395.6000f, 765.8500f, 0.3008f},  // /EnvironmentSetting/#34@3/#29@39/#28@7
    {Atlas::Common3, Blend::Add, false, true, 965.450f, 732.900f, 1354.450f, 738.900f, 0.154237f, 0.000000f, 0.000000f, -1.000000f, -106.1538f, 765.8500f, 0.3008f},  // /EnvironmentSetting/#34@3/#29@39/#28@7
    {Atlas::Common3, Blend::Add, false, true, 949.150f, 731.750f, 968.150f, 758.900f, 1.000000f, 0.000000f, 0.000000f, -1.000000f, -566.7000f, 772.4500f, 0.3008f},  // /EnvironmentSetting/#34@3/#29@39/#28@7
    {Atlas::Common3, Blend::Add, false, true, 907.650f, 737.350f, 952.300f, 749.850f, 1.000000f, 0.000000f, 0.000000f, -1.000000f, -833.3500f, 822.7500f, 0.3008f},  // /EnvironmentSetting/#34@3/#29@39/#28@7
    {Atlas::Common3, Blend::Add, false, true, 965.550f, 737.350f, 1010.200f, 749.850f, -1.000000f, 0.000000f, 0.000000f, -1.000000f, 1084.5000f, 822.7500f, 0.3008f},  // /EnvironmentSetting/#34@3/#29@39/#28@7
    {Atlas::Common3, Blend::Add, false, true, 955.250f, 268.700f, 961.300f, 385.650f, 0.000014f, 0.513049f, -1.000000f, 0.000305f, -95.1129f, 988.1435f, 0.1992f},  // /EnvironmentSetting/#34@3/#31@47/#30@1
    {Atlas::Common3, Blend::Add, false, true, 955.250f, 417.650f, 961.300f, 597.807f, 0.000014f, 0.333050f, -1.000000f, 0.000198f, -96.3549f, 988.1428f, 0.1992f},  // /EnvironmentSetting/#34@3/#31@49/#30@1
    {Atlas::Common3, Blend::Add, false, true, 725.850f, 151.700f, 1185.850f, 199.700f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -718.8000f, -93.2000f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@51/#32@5
    {Atlas::Common3, Blend::Add, false, true, 501.550f, 178.150f, 801.550f, 180.150f, 0.100005f, 0.000000f, 0.000000f, 1.000000f, 71.6885f, -94.3500f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@51/#32@5
    {Atlas::Common3, Blend::Add, false, true, 1117.550f, 178.150f, 1417.550f, 180.150f, 0.100005f, 0.000000f, 0.000000f, 1.000000f, 10.0855f, -94.3500f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@51/#32@5
    {Atlas::Common3, Blend::Add, false, true, 501.550f, 166.650f, 546.200f, 179.150f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -427.2500f, -93.7500f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@51/#32@5
    {Atlas::Common3, Blend::Add, false, true, 1373.400f, 166.650f, 1418.050f, 179.150f, -1.000000f, 0.000000f, 0.000000f, 1.000000f, 1492.3500f, -93.7500f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@51/#32@5
    {Atlas::Common3, Blend::Add, false, true, 725.850f, 843.650f, 1185.850f, 891.650f, 1.000000f, 0.000000f, 0.000000f, -1.000000f, -718.8000f, 950.1500f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@57/#32@5
    {Atlas::Common3, Blend::Add, false, true, 501.550f, 863.200f, 801.550f, 865.200f, 0.100005f, 0.000000f, 0.000000f, -1.000000f, 71.6885f, 949.0000f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@57/#32@5
    {Atlas::Common3, Blend::Add, false, true, 1117.550f, 863.200f, 1417.550f, 865.200f, 0.100005f, 0.000000f, 0.000000f, -1.000000f, 10.0855f, 949.0000f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@57/#32@5
    {Atlas::Common3, Blend::Add, false, true, 501.550f, 864.200f, 546.200f, 876.700f, 1.000000f, 0.000000f, 0.000000f, -1.000000f, -427.2500f, 949.6000f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@57/#32@5
    {Atlas::Common3, Blend::Add, false, true, 1373.400f, 864.200f, 1418.050f, 876.700f, -1.000000f, 0.000000f, 0.000000f, -1.000000f, 1492.3500f, 949.6000f, 1.0000f},  // /EnvironmentSetting/#34@3/#33@57/#32@5
};

inline constexpr Piece RowLine[] = {
    {Atlas::Common3, Blend::Add, false, true, -59.200f, 47.800f, 90.800f, 49.800f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, 70.4500f, 191.1500f, 0.1992f},  // /EnvironmentSetting/Item_0_0/#40@2/#39@3
    {Atlas::Common3, Blend::Add, false, true, 90.800f, 47.800f, 270.800f, 49.800f, 0.111108f, 0.000000f, 0.000000f, 1.000000f, 153.2620f, 191.1500f, 0.1992f},  // /EnvironmentSetting/Item_0_0/#40@2/#39@3
    {Atlas::Common3, Blend::Add, false, true, 270.800f, 47.800f, 340.800f, 49.800f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -146.4000f, 198.1500f, 0.1992f},  // /EnvironmentSetting/Item_0_0/#40@2/#39@3
};

inline constexpr Piece RowGlow[] = {
    {Atlas::Common3, Blend::Add, false, true, -220.150f, -15.850f, 271.900f, 92.150f, 0.721490f, 0.000000f, 0.000000f, 1.000000f, 165.7983f, 828.7500f, 1.0000f},  // /EnvironmentSetting/Item_0_0/Cursor/Cursor/#41@2
    {Atlas::Common3, Blend::Add, false, true, 271.700f, -15.850f, 392.750f, 92.150f, -2.932496f, 0.000000f, 0.000000f, 1.000000f, 1158.7757f, 828.7500f, 1.0000f},  // /EnvironmentSetting/Item_0_0/Cursor/Cursor/#41@2
};

inline constexpr Piece ComboOrnament[] = {
    {Atlas::Common3, Blend::Add, false, true, 449.400f, 48.250f, 494.050f, 60.750f, 1.000000f, 0.000000f, 0.000000f, -1.000000f, -375.1000f, 133.6500f, 0.3984f},  // combo/#45/#44@3
    {Atlas::Common3, Blend::Add, false, true, 685.300f, 48.250f, 729.950f, 60.750f, -1.000000f, 0.000000f, 0.000000f, -1.000000f, 804.2500f, 133.6500f, 0.3984f},  // combo/#45/#44@3
    {Atlas::Common3, Blend::Add, false, true, 454.100f, 46.750f, 725.100f, 48.750f, 0.110709f, 0.000000f, 0.000000f, 1.000000f, 71.5734f, 37.0500f, 0.3984f},  // combo/#45/#44@3
};

inline constexpr Piece ComboGlow[] = {
    {Atlas::Common3, Blend::Add, false, true, 232.500f, -18.050f, 587.500f, 89.947f, 1.000000f, 0.000000f, 0.000000f, 1.000031f, -225.5500f, 830.9506f, 0.8008f},  // combo/Cursor/glow/#48@2
    {Atlas::Common3, Blend::Add, false, true, 587.500f, -18.050f, 942.500f, 89.947f, -1.000000f, 0.000000f, 0.000000f, 1.000031f, 949.4500f, 830.9506f, 0.8008f},  // combo/Cursor/glow/#48@2
};

inline constexpr Piece ComboArrowLeft[] = {
    {Atlas::Common3, Blend::Add, false, true, 447.800f, 20.350f, 458.800f, 42.350f, -1.000000f, 0.000000f, 0.000000f, 1.000000f, 656.4500f, 90.1500f, 1.0000f},  // combo/Cursor/left/#54@1
};

inline constexpr Piece ComboArrowRight[] = {
    {Atlas::Common3, Blend::Add, false, true, 721.300f, 20.350f, 732.300f, 42.350f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -523.6500f, 90.1500f, 1.0000f},  // combo/Cursor/right/#54@1
};

inline constexpr Piece SliderBox[] = {
    {Atlas::Common3, Blend::Add, false, true, 738.600f, 15.950f, 791.600f, 19.100f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -524.1000f, 94.9000f, 0.5000f},  // slider/#62/#59@1/#58@1
    {Atlas::Common3, Blend::Add, false, true, 738.600f, 19.100f, 742.100f, 47.100f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -524.1000f, 94.9000f, 0.5000f},  // slider/#62/#59@1/#58@1
    {Atlas::Common3, Blend::Add, false, true, 788.100f, 19.100f, 791.600f, 47.100f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -524.1000f, 94.9000f, 0.5000f},  // slider/#62/#59@1/#58@1
    {Atlas::Common3, Blend::Add, false, true, 738.600f, 47.100f, 791.600f, 49.950f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -524.1000f, 94.9000f, 0.5000f},  // slider/#62/#59@1/#58@1
    {Atlas::Common3, Blend::Over, false, true, 742.150f, 19.100f, 788.150f, 47.100f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -524.1500f, 94.9000f, 0.3984f},  // slider/#62/#61@3/#60@1
};

inline constexpr Piece SliderTrack[] = {
    {Atlas::Common1, Blend::Over, false, true, 422.748f, 25.801f, 733.750f, 39.714f, 1.000015f, -0.000443f, 0.000443f, 1.000015f, 1143.2566f, 1507.0110f, 1.0000f},  // slider/Bar/track/#68@1
};

inline constexpr Piece SliderFill[] = {
    {Atlas::Common1, Blend::Over, false, true, 426.000f, 29.450f, 730.998f, 36.315f, 1.000015f, -0.000443f, 0.000443f, 1.000015f, 1140.0066f, 1521.3610f, 1.0000f},  // slider/Bar/fill/#70@1
};

inline constexpr Piece SliderArrowLeft[] = {
    {Atlas::Common3, Blend::Add, false, true, 400.700f, 22.250f, 411.700f, 44.250f, -1.000000f, 0.000000f, 0.000000f, -1.000000f, 609.3500f, 154.7500f, 1.0000f},  // slider/LeftArrow/#55@1/#54@1
};

inline constexpr Piece SliderArrowRight[] = {
    {Atlas::Common3, Blend::Add, false, true, 805.400f, 22.250f, 816.400f, 44.250f, 1.000000f, 0.000000f, 0.000000f, 1.000000f, -607.7500f, 88.2500f, 1.0000f},  // slider/RightArrow/#55@1/#54@1
};

inline constexpr Piece SliderGlow[] = {
    {Atlas::Common3, Blend::Add, false, true, 189.350f, -28.500f, 599.350f, 111.500f, 0.865850f, 0.000000f, 0.000000f, 0.771429f, -156.9786f, 834.8793f, 1.0000f},  // slider/Cursor/#66@1/#65@2
    {Atlas::Common3, Blend::Add, false, true, 599.500f, -28.500f, 1009.500f, 111.500f, -0.865790f, 0.000000f, 0.000000f, 0.771429f, 880.9850f, 834.8793f, 1.0000f},  // slider/Cursor/#66@1/#65@2
};

inline constexpr Piece ButtonOrnament[] = {
    {Atlas::Common3, Blend::Add, false, true, 0.000f, 0.000f, 44.650f, 12.500f, 1.000000f, 0.000000f, 0.000000f, -1.000000f, 74.3000f, 85.4000f, 0.3984f},  // button/#98/#44@3
    {Atlas::Common3, Blend::Add, false, true, 235.900f, 0.000f, 280.550f, 12.500f, -1.000000f, 0.000000f, 0.000000f, -1.000000f, 354.8500f, 85.4000f, 0.3984f},  // button/#98/#44@3
    {Atlas::Common3, Blend::Add, false, true, 4.700f, -1.500f, 275.700f, 0.500f, 0.110709f, 0.000000f, 0.000000f, 1.000000f, 121.3260f, 85.3000f, 0.3984f},  // button/#98/#44@3
};

inline constexpr Piece ButtonGlow[] = {
    {Atlas::Common3, Blend::Add, false, true, -217.050f, -76.150f, 137.950f, 53.852f, 1.000000f, 0.000000f, 0.000000f, 0.830758f, 224.0000f, 876.1622f, 1.0000f},  // button/Cursor/#49@1/#48@2
    {Atlas::Common3, Blend::Add, false, true, 137.950f, -76.150f, 492.950f, 53.852f, -1.000000f, 0.000000f, 0.000000f, 0.830758f, 499.9000f, 876.1622f, 1.0000f},  // button/Cursor/#49@1/#48@2
};

} // namespace VR::PageArt
