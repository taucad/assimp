# USD model files

## MotionCaptureROM.usdc
Converted bundled .wrl test model:
    https://github.com/assimp/assimp/blob/master/test/models/WRL/MotionCaptureROM.WRL
to .usdc format via website:
    https://imagetostl.com/convert/file/wrl/to/usd#convert
(As of Oct 2024, animations have not been verified as present or correct)

## Fixtures from LightUSD (Apache-2.0)
Copied from LightUSD (https://github.com/lighttransport/LightUSD, commit 984f05b of
the taucad fork), licensed under the Apache License 2.0,
Copyright (c) 2020-2023 Syoyo Fujita, (c) 2024-Present Light Transport Entertainment Inc.
- [usda/physics-joint.usda](usda/physics-joint.usda): LightUSD `tests/usda/physics-joint.usda`
- [usda/blender-physics.usda](usda/blender-physics.usda): LightUSD `models/blender-physics.usda`
- [mtlx/basic_openpbr.mtlx](mtlx/basic_openpbr.mtlx): LightUSD `tests/feat/mtlx/data/basic_openpbr.mtlx`
- [nested-look-texture/](nested-look-texture/): LightUSD `models/nested-look-texture`
  (root layer, look layer under `look/`, texture under `tex/`)
