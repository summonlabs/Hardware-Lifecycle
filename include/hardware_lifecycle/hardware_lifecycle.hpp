#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Umbrella header. Prefer including the narrow header you actually need:
// lifecycle.hpp for the state machine, requests.hpp for the request and receipt
// types, persistence.hpp for the durable store, runtime.hpp for the composed
// runtime.

#include "hardware_lifecycle/compatibility.hpp"
#include "hardware_lifecycle/diff.hpp"
#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/export.hpp"
#include "hardware_lifecycle/health.hpp"
#include "hardware_lifecycle/history.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/persistence.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/registry.hpp"
#include "hardware_lifecycle/replacement.hpp"
#include "hardware_lifecycle/requests.hpp"
#include "hardware_lifecycle/result.hpp"
#include "hardware_lifecycle/runtime.hpp"
#include "hardware_lifecycle/text.hpp"
#include "hardware_lifecycle/version.hpp"
