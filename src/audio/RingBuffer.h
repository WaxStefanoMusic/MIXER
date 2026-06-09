#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace mixer::audio {

// SPSC (single-producer single-consumer) lock-free ring buffer di float.
// Pensato per audio: il thread capture scrive (push), il thread render legge (pop).
// Capacit arrotondata alla potenza di 2 superiore per modulo via bitmask.
//
// Misurato in SAMPLE (non frame): se hai 2 canali interlacciati, scrivi
// frames*2 sample. La classe non conosce il channel count, solo sample.
class RingBuffer
{
public:
    explicit RingBuffer(size_t requested_samples)
    {
        size_t cap = 1;
        while (cap < requested_samples) cap <<= 1;
        buf_.assign(cap, 0.0f);
        mask_ = cap - 1;
    }

    // Capacit utile = capacit-1 (un slot riservato per distinguere full/empty).
    size_t capacity() const noexcept { return buf_.size() - 1; }

    size_t readAvailable() const noexcept
    {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_relaxed);
        return (h - t) & mask_;
    }

    size_t writeAvailable() const noexcept
    {
        return capacity() - readAvailable();
    }

    // Scrive fino a n sample. Ritorna quanti ne ha effettivamente scritti.
    // Se non c' spazio, ritorna meno (overflow: l'eccesso viene scartato).
    size_t push(const float* src, size_t n) noexcept
    {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t free_space = capacity() - ((h - t) & mask_);
        const size_t to_write = (n < free_space) ? n : free_space;

        const size_t cap = buf_.size();
        const size_t h_idx = h & mask_;
        const size_t first = (h_idx + to_write <= cap) ? to_write : (cap - h_idx);
        std::memcpy(&buf_[h_idx], src, first * sizeof(float));
        if (first < to_write)
            std::memcpy(&buf_[0], src + first, (to_write - first) * sizeof(float));

        head_.store(h + to_write, std::memory_order_release);
        return to_write;
    }

    // Legge fino a n sample. Ritorna quanti ne ha effettivamente letti.
    // Se non c' abbastanza, ritorna meno (underrun: chiamante deve zero-fill).
    size_t pop(float* dst, size_t n) noexcept
    {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t avail = (h - t) & mask_;
        const size_t to_read = (n < avail) ? n : avail;

        const size_t cap = buf_.size();
        const size_t t_idx = t & mask_;
        const size_t first = (t_idx + to_read <= cap) ? to_read : (cap - t_idx);
        std::memcpy(dst, &buf_[t_idx], first * sizeof(float));
        if (first < to_read)
            std::memcpy(dst + first, &buf_[0], (to_read - first) * sizeof(float));

        tail_.store(t + to_read, std::memory_order_release);
        return to_read;
    }

    void reset() noexcept
    {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    // alignas(64) per evitare false sharing tra head e tail su cache line diverse.
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace mixer::audio
