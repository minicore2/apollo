/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#pragma once

#include "modules/storytelling/proto/story.pb.h"
#include "modules/storytelling/proto/storytelling_config.pb.h"

namespace apollo {
namespace storytelling {

class BaseTeller {
 public:
  virtual ~BaseTeller() = default;
  virtual void Init(const StorytellingConfig& storytelling_conf) = 0;
  virtual void Update(Stories* stories) = 0;
};

}  // namespace storytelling
}  // namespace apollo
