// Copyright 2024 Khalil Estell
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <libhal-rmd/mc_x.hpp>

#include <cstdint>

#include <libhal-util/can.hpp>
#include <libhal-util/enum.hpp>
#include <libhal-util/map.hpp>
#include <libhal-util/steady_clock.hpp>

#include "common.hpp"
#include "mc_x_constants.hpp"

namespace hal::rmd {

struct response_waiter
{
  using message_t = decltype(mc_x::feedback_t::message_number);
  response_waiter(mc_x* p_this)
    : m_this(p_this)
  {
    m_original_message_number = m_this->feedback().message_number;
  }
  hal::status wait()
  {
    auto timeout =
      hal::create_timeout(*m_this->m_clock, m_this->m_max_response_time);
    while (true) {
      if (m_original_message_number != m_this->feedback().message_number) {
        return hal::success();
      }
      HAL_CHECK(timeout());
    }
  }
  message_t m_original_message_number{ 0 };
  mc_x* m_this;
};

template<std::integral T>
result<T> bounds_check(std::floating_point auto p_float)
{
  using float_t = decltype(p_float);
  constexpr auto min = static_cast<float_t>(std::numeric_limits<T>::min());
  constexpr auto max = static_cast<float_t>(std::numeric_limits<T>::max());

  if (min < p_float && p_float < max) {
    return static_cast<T>(p_float);
  }

  return new_error(std::errc::result_out_of_range);
}

hal::ampere mc_x::feedback_t::current() const noexcept
{
  static constexpr auto amps_per_lsb = 0.1_A;
  return static_cast<float>(raw_current) * amps_per_lsb;
}

hal::rpm mc_x::feedback_t::speed() const noexcept
{
  static constexpr auto velocity_per_lsb = 1.0_deg_per_sec;
  return static_cast<float>(raw_speed) * velocity_per_lsb;
}

hal::volts mc_x::feedback_t::volts() const noexcept
{
  static constexpr float volts_per_lsb = 0.1f;
  return static_cast<float>(raw_volts) * volts_per_lsb;
}

hal::celsius mc_x::feedback_t::temperature() const noexcept
{
  static constexpr float celsius_per_lsb = 1.0f;
  return static_cast<float>(raw_motor_temperature) * celsius_per_lsb;
}

hal::degrees mc_x::feedback_t::angle() const noexcept
{
  return static_cast<float>(raw_multi_turn_angle) * dps_per_lsb_speed;
}

bool mc_x::feedback_t::motor_stall() const noexcept
{
  return raw_error_state & motor_stall_mask;
}
bool mc_x::feedback_t::low_pressure() const noexcept
{
  return raw_error_state & low_pressure_mask;
}
bool mc_x::feedback_t::over_voltage() const noexcept
{
  return raw_error_state & over_voltage_mask;
}
bool mc_x::feedback_t::over_current() const noexcept
{
  return raw_error_state & over_current_mask;
}
bool mc_x::feedback_t::power_overrun() const noexcept
{
  return raw_error_state & power_overrun_mask;
}
bool mc_x::feedback_t::speeding() const noexcept
{
  return raw_error_state & speeding_mask;
}
bool mc_x::feedback_t::over_temperature() const noexcept
{
  return raw_error_state & over_temperature_mask;
}
bool mc_x::feedback_t::encoder_calibration_error() const noexcept
{
  return raw_error_state & encoder_calibration_error_mask;
}

result<mc_x> mc_x::create(hal::can_router& p_router,
                          hal::steady_clock& p_clock,
                          float p_gear_ratio,
                          can::id_t device_id,
                          hal::time_duration p_max_response_time)
{
  mc_x mc_x_driver(
    p_router, p_clock, p_gear_ratio, device_id, p_max_response_time);
  return mc_x_driver;
}

mc_x::mc_x(mc_x&& p_other) noexcept
  : m_feedback(p_other.m_feedback)
  , m_clock(p_other.m_clock)
  , m_router(p_other.m_router)
  , m_route_item(std::move(p_other.m_route_item))
  , m_gear_ratio(p_other.m_gear_ratio)
  , m_device_id(p_other.m_device_id)
  , m_max_response_time(p_other.m_max_response_time)
{
  m_route_item.get().handler = std::ref(*this);
}

mc_x& mc_x::operator=(mc_x&& p_other) noexcept
{
  m_feedback = p_other.m_feedback;
  m_clock = p_other.m_clock;
  m_router = p_other.m_router;
  m_route_item = std::move(p_other.m_route_item);
  m_gear_ratio = p_other.m_gear_ratio;
  m_device_id = p_other.m_device_id;
  m_max_response_time = p_other.m_max_response_time;

  m_route_item.get().handler = std::ref(*this);

  return *this;
}

const mc_x::feedback_t& mc_x::feedback() const
{
  return m_feedback;
}

mc_x::mc_x(hal::can_router& p_router,
           hal::steady_clock& p_clock,
           float p_gear_ratio,  // NOLINT
           can::id_t p_device_id,
           hal::time_duration p_max_response_time)
  : m_feedback{}
  , m_clock(&p_clock)
  , m_router(&p_router)
  , m_route_item(
      p_router.add_message_callback(p_device_id + response_id_offset))
  , m_gear_ratio(p_gear_ratio)
  , m_device_id(p_device_id)
  , m_max_response_time(p_max_response_time)
{
  m_route_item.get().handler = std::ref(*this);
}

result<std::int32_t> rpm_to_mc_x_speed(rpm p_rpm, float p_dps_per_lsb)
{
  static constexpr float dps_per_rpm = (1.0f / 1.0_deg_per_sec);
  const float dps_float = (p_rpm * dps_per_rpm) / p_dps_per_lsb;
  return bounds_check<std::int32_t>(dps_float);
}

status mc_x::velocity_control(rpm p_rpm)
{
  response_waiter response(this);

  const auto speed_data =
    HAL_CHECK(rpm_to_mc_x_speed(p_rpm, dps_per_lsb_speed));

  HAL_CHECK(m_router->bus().send(
    message(m_device_id,
            {
              hal::value(actuate::speed),
              0x00,
              0x00,
              0x00,
              static_cast<hal::byte>((speed_data >> 0) & 0xFF),
              static_cast<hal::byte>((speed_data >> 8) & 0xFF),
              static_cast<hal::byte>((speed_data >> 16) & 0xFF),
              static_cast<hal::byte>((speed_data >> 24) & 0xFF),
            })));

  return response.wait();
}

status mc_x::position_control(degrees p_angle, rpm p_rpm)  // NOLINT
{
  response_waiter response(this);

  static constexpr float deg_per_lsb = 0.01f;
  const auto angle = p_angle / deg_per_lsb;
  const auto angle_data = HAL_CHECK(bounds_check<std::int32_t>(angle));
  const auto speed_data = HAL_CHECK(
    rpm_to_mc_x_speed(std::abs(p_rpm * m_gear_ratio), dps_per_lsb_angle));

  HAL_CHECK(m_router->bus().send(
    message(m_device_id,
            {
              hal::value(actuate::position),
              0x00,
              static_cast<hal::byte>((speed_data >> 0) & 0xFF),
              static_cast<hal::byte>((speed_data >> 8) & 0xFF),
              static_cast<hal::byte>((angle_data >> 0) & 0xFF),
              static_cast<hal::byte>((angle_data >> 8) & 0xFF),
              static_cast<hal::byte>((angle_data >> 16) & 0xFF),
              static_cast<hal::byte>((angle_data >> 24) & 0xFF),
            })));

  return response.wait();
}

status mc_x::feedback_request(read p_command)
{
  response_waiter response(this);

  HAL_CHECK(m_router->bus().send(message(m_device_id,
                                         {
                                           hal::value(p_command),
                                           0x00,
                                           0x00,
                                           0x00,
                                           0x00,
                                           0x00,
                                           0x00,
                                           0x00,
                                         })));

  return response.wait();
}

status mc_x::system_control(system p_system_command)
{
  response_waiter response(this);

  HAL_CHECK(m_router->bus().send(message(m_device_id,
                                         {
                                           hal::value(p_system_command),
                                           0x00,
                                           0x00,
                                           0x00,
                                           0x00,
                                           0x00,
                                           0x00,
                                           0x00,
                                         })));

  return response.wait();
}

void mc_x::operator()(const can::message_t& p_message)
{
  m_feedback.message_number++;

  if (p_message.length != 8 ||
      p_message.id != m_device_id + response_id_offset) {
    return;
  }

  switch (p_message.payload[0]) {
    case hal::value(read::status_2):
    case hal::value(actuate::torque):
    case hal::value(actuate::speed):
    case hal::value(actuate::position): {
      auto& data = p_message.payload;
      m_feedback.raw_motor_temperature = static_cast<int8_t>(data[1]);
      m_feedback.raw_current = static_cast<int16_t>((data[3] << 8) | data[2]);
      m_feedback.raw_speed = static_cast<int16_t>((data[5] << 8) | data[4]);
      m_feedback.encoder = static_cast<int16_t>((data[7] << 8) | data[6]);
      break;
    }
    case hal::value(read::status_1_and_error_flags): {
      auto& data = p_message.payload;
      m_feedback.raw_motor_temperature = static_cast<int8_t>(data[1]);
      // data[3] = Brake release command
      m_feedback.raw_volts = static_cast<int16_t>((data[5] << 8) | data[4]);
      auto error_state = data[7] << 8 | data[6];
      m_feedback.raw_error_state = static_cast<int16_t>(error_state);
      break;
    }
    case hal::value(read::multi_turns_angle): {
      auto& data = p_message.payload;
      m_feedback.raw_multi_turn_angle = static_cast<std::int32_t>(
        data[7] << 24 | data[6] << 16 | data[5] << 8 | data[4]);
      break;
    }
    default:
      return;
  }

  return;
}
}  // namespace hal::rmd
