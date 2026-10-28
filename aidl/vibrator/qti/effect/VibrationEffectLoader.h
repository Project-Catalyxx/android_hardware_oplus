/*
 * Copyright (C) 2025 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "effect.h"

#include <json/json.h>
#include <unordered_map>

class VibrationEffectLoader {
  public:
    VibrationEffectLoader();
    ~VibrationEffectLoader();
    effect_stream* getEffectStream(uint32_t effect_id);

  private:
    using EffectMap = std::unordered_map<uint32_t, effect_stream>;

    Json::Value parseEffectJson(std::ifstream& config_stream);
    void loadEffects(Json::Value&& effect_nodes, EffectMap& effect_map);

    EffectMap effect_map_;
    EffectMap soft_effect_map_;
};
