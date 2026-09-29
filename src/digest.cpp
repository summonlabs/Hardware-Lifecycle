// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SHA-256 (FIPS 180-4) as the authoritative binding primitive, and the strict
// digest value type built on it. The running state of a hasher is never
// destroyed by finalisation, so one hasher can produce a prefix digest and then
// continue with more input.

#include "hardware_lifecycle/digest.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"
#include "hardware_lifecycle/text.hpp"

namespace hardware_lifecycle {
namespace {

/// First 32 bits of the fractional parts of the cube roots of the first 64
/// primes (FIPS 180-4 section 4.2.2).
constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

/// First 32 bits of the fractional parts of the square roots of the first eight
/// primes (FIPS 180-4 section 5.3.3).
constexpr std::array<std::uint32_t, 8> kInitialState = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

/// One SHA-256 compression round over a single 64 byte block. The state is
/// updated in place; the block is read only.
void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t base = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24) |
                      (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                      static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t previous_15 = schedule[index - 15];
    const std::uint32_t previous_2 = schedule[index - 2];
    const std::uint32_t sigma_0 =
        std::rotr(previous_15, 7) ^ std::rotr(previous_15, 18) ^ (previous_15 >> 3);
    const std::uint32_t sigma_1 = std::rotr(previous_2, 17) ^ std::rotr(previous_2, 19) ^ (previous_2 >> 10);
    schedule[index] = schedule[index - 16] + sigma_0 + schedule[index - 7] + sigma_1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];
  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t big_sigma_1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp_1 = h + big_sigma_1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t big_sigma_0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp_2 = big_sigma_0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp_1;
    d = c;
    c = b;
    b = a;
    a = temp_1 + temp_2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

Sha256::Sha256() noexcept : state_(kInitialState) {}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return;
  }
  const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += static_cast<std::uint64_t>(size);

  std::size_t offset = 0;
  if (buffered_ != 0) {
    const std::size_t free_bytes = buffer_.size() - buffered_;
    const std::size_t taken = size < free_bytes ? size : free_bytes;
    for (std::size_t index = 0; index < taken; ++index) {
      buffer_[buffered_ + index] = bytes[index];
    }
    buffered_ += taken;
    offset = taken;
    if (buffered_ == buffer_.size()) {
      compress(state_, buffer_.data());
      buffered_ = 0;
    }
  }
  while (size - offset >= buffer_.size()) {
    compress(state_, bytes + offset);
    offset += buffer_.size();
  }
  while (offset < size) {
    buffer_[buffered_] = bytes[offset];
    ++buffered_;
    ++offset;
  }
}

void Sha256::update(std::string_view text) noexcept { update(text.data(), text.size()); }

Digest Sha256::finish() const noexcept {
  // Finalisation works on copies: the running state stays exactly as it was, so
  // a caller can take a digest of a prefix and keep feeding the same hasher.
  std::array<std::uint32_t, 8> state = state_;
  std::array<std::uint8_t, 64> block = buffer_;
  std::size_t length = buffered_;

  block[length] = static_cast<std::uint8_t>(0x80u);
  ++length;
  if (length > 56) {
    while (length < block.size()) {
      block[length] = static_cast<std::uint8_t>(0);
      ++length;
    }
    compress(state, block.data());
    length = 0;
  }
  while (length < 56) {
    block[length] = static_cast<std::uint8_t>(0);
    ++length;
  }
  // The length field is the message length in bits, big endian, taken modulo
  // 2^64 as the standard prescribes.
  const std::uint64_t bits = total_bytes_ * 8u;
  for (std::size_t index = 0; index < 8; ++index) {
    block[56 + index] = static_cast<std::uint8_t>((bits >> (56u - index * 8u)) & 0xFFu);
  }
  compress(state, block.data());

  std::array<std::uint8_t, Digest::kSize> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index * 4] = static_cast<std::uint8_t>(state[index] >> 24);
    bytes[index * 4 + 1] = static_cast<std::uint8_t>(state[index] >> 16);
    bytes[index * 4 + 2] = static_cast<std::uint8_t>(state[index] >> 8);
    bytes[index * 4 + 3] = static_cast<std::uint8_t>(state[index]);
  }
  return Digest::from_bytes(bytes);
}

Digest Sha256::hash(const void* data, std::size_t size) noexcept {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

Digest Sha256::hash(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

Digest Digest::from_bytes(const std::array<std::uint8_t, kSize>& bytes) noexcept {
  Digest digest;
  digest.bytes_ = bytes;
  return digest;
}

Result<Digest> Digest::from_hex(std::string_view text) {
  if (text.size() != kSize * 2) {
    return Error::make(ErrorCode::MalformedRequest, "a digest is exactly 64 hexadecimal characters")
        .with("digest", std::string(text));
  }
  // Qualified call: the free decoder is the one that takes a byte budget.
  const Result<std::vector<std::uint8_t>> decoded = hardware_lifecycle::from_hex(text, kSize);
  if (!decoded.has_value()) {
    return decoded.error();
  }
  std::array<std::uint8_t, kSize> bytes{};
  for (std::size_t index = 0; index < kSize; ++index) {
    bytes[index] = decoded.value()[index];
  }
  return from_bytes(bytes);
}

std::string Digest::hex() const { return to_hex_lower(bytes_.data(), bytes_.size()); }

bool Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace hardware_lifecycle
