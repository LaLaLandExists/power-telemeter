/**
 * classifier_features.h
 * Feature vector produced by the AFE DSP pipeline every 2 seconds.
 *
 * ClassifierFeatures is written by runHarmonicAnalysis() in meter_afe.cpp and
 * exposed via g_features / g_featuresMutex for any future load-classifier
 * module. Nothing in the TDMA path (node_tdma_common.cpp) references this
 * header -- the coupling is strictly opt-in.
 *
 * To consume: #include "classifier_features.h", take g_featuresMutex, read.
 */
#pragma once
#include "hal/hal_rtos.h"

struct ClassifierFeatures {
  float pf;       // f0: total power factor [0..1] (magnitude of P/S)
  float qSign;    // f1: reactive power polarity: +1 inductive, -1 capacitive, 0
  float thdI;     // f2: current THD (odd harmonics), dimensionless
  float h3Ratio;  // f3: |I3| / |I1|
  float h5Ratio;  // f4: |I5| / |I1|
  float h7Ratio;  // f5: |I7| / |I1|
  float cf;       // f6: crest factor  Ipeak / Irms
  float oeRatio;  // f7: even harmonic ratio  (|I2|+|I4|) / |I1|
  float h53Ratio; // f8: |I5| / |I3|
  float pRms;     // f9: log2(Irms / 0.05), clamped [0, 10]
};

#ifdef METER_AFE
extern ClassifierFeatures g_features;
extern SemaphoreHandle_t  g_featuresMutex;
#endif
