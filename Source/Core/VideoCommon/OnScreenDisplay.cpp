// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/OnScreenDisplay.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Timer.h"

#include "Core/Config/MainSettings.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConfig.h"

namespace OSD
{
constexpr float LEFT_MARGIN = 10.0f;         // Pixels to the left of OSD messages.
constexpr float TOP_MARGIN = 10.0f;          // Pixels above the first OSD message.
constexpr float WINDOW_PADDING = 4.0f;       // Pixels between subsequent OSD messages.
constexpr float MESSAGE_FADE_TIME = 1000.f;  // Ms to fade OSD messages at the end of their life.
constexpr float MESSAGE_DROP_TIME = 5000.f;  // Ms to drop OSD messages that has yet to ever render.

static std::atomic<int> s_obscured_pixels_left = 0;
static std::atomic<int> s_obscured_pixels_top = 0;

struct Message
{
  Message() = default;
  Message(std::string text_, u32 duration_, u32 color_, std::unique_ptr<Icon> icon_ = nullptr,
          float scale_ = 1)
      : text(std::move(text_)), duration(duration_), color(color_), icon(std::move(icon_)),
        scale(scale_)
  {
    timer.Start();
  }
  s64 TimeRemaining() const { return duration - timer.ElapsedMs(); }
  std::string text;
  Common::Timer timer;
  u32 duration = 0;
  bool ever_drawn = false;
  bool should_discard = false;
  u32 color = 0;
  std::unique_ptr<Icon> icon;
  std::unique_ptr<AbstractTexture> texture;
  float scale = 1;
};

struct OSDMessageStack
{
  ImVec2 initialPosOffset;
  MessageStackDirection dir;
  bool centered;
  bool reversed;
  std::string name;
  std::multimap<OSD::MessageType, OSD::Message> messages;

  OSDMessageStack() : OSDMessageStack(0, 0, MessageStackDirection::Downward, false, false, "") {}
  OSDMessageStack(float x_offset, float y_offset, MessageStackDirection dir, bool centered,
                  bool reversed, std::string name)
      : dir(dir), centered(centered), reversed(reversed), name(name)
  {
    initialPosOffset = ImVec2(x_offset, y_offset);
  }

  bool IsVertical()
  {
    return dir == MessageStackDirection::Downward || dir == MessageStackDirection::Upward;
  }

  bool HasMessage(std::string message, MessageType type = OSD::MessageType::Typeless)
  {
    for (auto it = messages.begin(); it != messages.end(); ++it)
    {
      if (type == it->first && message == it->second.text)
      {
        return true;
      }
    }
    return false;
  }
};

// default message stack
static OSDMessageStack s_defaultMessageStack = OSDMessageStack();
static std::map<std::string, OSDMessageStack> messageStacks;

static std::mutex s_messages_mutex;

static ImVec4 ARGBToImVec4(const u32 argb)
{
  return ImVec4(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                static_cast<float>((argb >> 0) & 0xFF) / 255.0f,
                static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
}

static ImVec2 DrawMessage(int index, Message& msg, const ImVec2& position, int time_left,
                          OSDMessageStack& message_Stack)
{
  // We have to provide a window name, and these shouldn't be duplicated.
  // So instead, we generate a name based on the number of messages drawn.
  const std::string window_name = fmt::format("osd_{}_{}", message_Stack.name, index);

  // The size must be reset, otherwise the length of old messages could influence new ones.
  ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));

  // Gradually fade old messages away (except in their first frame)
  const float fade_time = std::max(std::min(MESSAGE_FADE_TIME, (float)msg.duration), 1.f);
  const float alpha = std::clamp(time_left / fade_time, 0.f, 1.f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, msg.ever_drawn ? alpha : 1.0);

  float window_height = 0.0f;
  float window_width = 0.0f;
  if (ImGui::Begin(window_name.c_str(), nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    if (msg.icon)
    {
      if (!msg.texture)
      {
        const u32 width = msg.icon->width;
        const u32 height = msg.icon->height;
        TextureConfig tex_config(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0);
        msg.texture = g_gfx->CreateTexture(tex_config);
        if (msg.texture)
        {
          msg.texture->Load(0, width, height, width, msg.icon->rgba_data.data(),
                            sizeof(u32) * width * height);
        }
        else
        {
          // don't try again next time
          msg.icon.reset();
        }
      }

      if (msg.texture)
      {
        ImGui::Image(msg.texture.get(), ImVec2(static_cast<float>(msg.icon->width),
                                               static_cast<float>(msg.icon->height)));
      }
    }

    //TODO fractional scaling based on viewport size instead of screen pixels?
    ImGui::SetWindowFontScale(msg.scale);
    // Use %s in case message contains %.
    ImGui::TextColored(ARGBToImVec4(msg.color), "%s", msg.text.c_str());
    window_width =
        ImGui::GetWindowSize().x + (WINDOW_PADDING * ImGui::GetIO().DisplayFramebufferScale.x);
    window_height =
        ImGui::GetWindowSize().y + (WINDOW_PADDING * ImGui::GetIO().DisplayFramebufferScale.y);

    float x_pos = position.x;
    float y_pos = position.y;

    if (message_Stack.centered)
    {
      if (message_Stack.IsVertical())
      {
        const float x_center = ImGui::GetIO().DisplaySize.x / 2.0;
        x_pos = x_center - window_width / 2;
      }
      else
      {
        const float y_center = ImGui::GetIO().DisplaySize.y / 2.0;
        y_pos = y_center - window_height / 2;
      }
    }

    if (message_Stack.dir == MessageStackDirection::Leftward)
    {
      x_pos -= window_width;
    }
    if (message_Stack.dir == MessageStackDirection::Upward)
    {
      y_pos -= window_height;
    }

    const auto windowPos = ImVec2(x_pos, y_pos);
    ImGui::SetWindowPos(window_name.c_str(), windowPos);
  }

  ImGui::End();
  ImGui::PopStyleVar();

  msg.ever_drawn = true;

  return ImVec2(window_width, window_height);
}

void AddTypedMessage(MessageType type, std::string message, u32 ms, u32 argb, std::unique_ptr<Icon> icon,
                     std::string message_stack, bool prevent_duplicate, float scale)
{
  std::lock_guard lock{s_messages_mutex};

  OSDMessageStack* stack = &s_defaultMessageStack;
  if (messageStacks.contains(message_stack))
  {
    stack = &messageStacks[message_stack];
  }
    
  if (prevent_duplicate && stack->HasMessage(message, type))
  {
    return;
  }
  if (type != MessageType::Typeless)
  {
    // A message may hold a reference to a texture that can only be destroyed on the video thread, so
    // only mark the old typed message (if any) for removal. It will be discarded on the next call to
    // DrawMessages().
    auto range = stack->messages.equal_range(type);
    for (auto it = range.first; it != range.second; ++it)
        it->second.should_discard = true;
  }
  stack->messages.emplace(type, Message(std::move(message), ms, argb, std::move(icon), scale));
}

void AddMessage(std::string message, u32 ms, u32 argb, std::unique_ptr<Icon> icon, std::string message_stack, bool prevent_duplicate, float scale)
{
  AddTypedMessage(MessageType::Typeless, message, ms, argb, std::move(icon), message_stack,
                  prevent_duplicate,
                  scale);
}

void AddMessageStack(float x_offset, float y_offset, MessageStackDirection dir, bool centered,
                     bool reversed, std::string name)
{
  messageStacks.emplace(name, OSDMessageStack(x_offset, y_offset, dir, centered, reversed, name));
}
void DrawMessages(OSDMessageStack& messageStack)
{
  const bool draw_messages = Config::Get(Config::MAIN_OSD_MESSAGES);
  float current_x = LEFT_MARGIN * ImGui::GetIO().DisplayFramebufferScale.x +
                    s_obscured_pixels_left + messageStack.initialPosOffset.x;
  float current_y = TOP_MARGIN * ImGui::GetIO().DisplayFramebufferScale.y + s_obscured_pixels_top +
                    messageStack.initialPosOffset.y;
  int index = 0;

  if (messageStack.dir == MessageStackDirection::Leftward)
  {
    current_x = ImGui::GetIO().DisplaySize.x - current_x;
  }
  if (messageStack.dir == MessageStackDirection::Upward)
  {
    current_y = ImGui::GetIO().DisplaySize.y - current_y;
  }

  std::lock_guard lock{s_messages_mutex};
  for (auto it = (messageStack.reversed ? messageStack.messages.end() :
                                          messageStack.messages.begin());
       it !=
       (messageStack.reversed ? messageStack.messages.begin() : messageStack.messages.end());)
  {
    if (messageStack.reversed)
    {
      --it;
    }

    Message& msg = it->second;
    if (msg.should_discard)
    {
      it = messageStack.messages.erase(it);
      continue;
    }

    const s64 time_left = msg.TimeRemaining();

    // Make sure we draw them at least once if they were printed with 0ms,
    // unless enough time has expired, in that case, we drop them
    if (time_left <= 0 && (msg.ever_drawn || -time_left >= MESSAGE_DROP_TIME))
    {
      it = messageStack.messages.erase(it);
      continue;
    }
    else if (!messageStack.reversed)
    {
      ++it;
    }

    if (draw_messages)
    {
      const auto messageSize =
          DrawMessage(index++, msg, ImVec2(current_x, current_y), time_left, messageStack);

      if (messageStack.IsVertical())
      {
        current_y +=
            messageStack.dir == OSD::MessageStackDirection::Upward ? -messageSize.y : messageSize.y;
      }
      else
      {
        current_x += messageStack.dir == OSD::MessageStackDirection::Leftward ? -messageSize.x :
                                                                                messageSize.x;
      }
    }
  }
}
void DrawMessages()
{
  DrawMessages(s_defaultMessageStack);
  for (auto& [name, stack] : messageStacks)
  {
    DrawMessages(stack);
  }
}

void ClearMessages()
{
  std::lock_guard lock{s_messages_mutex};
  s_defaultMessageStack.messages.clear();
  for (auto& [name, stack] : messageStacks)
  {
    stack.messages.clear();
  }
}

void SetObscuredPixelsLeft(int width)
{
  s_obscured_pixels_left = width;
}

void SetObscuredPixelsTop(int height)
{
  s_obscured_pixels_top = height;
}
}  // namespace OSD
