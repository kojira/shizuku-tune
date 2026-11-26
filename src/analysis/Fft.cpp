#include "Fft.h"

#include <cmath>
#include <stdexcept>

namespace shizuku
{

int Fft::nextPowerOfTwo (int value) noexcept
{
    int result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

Fft::Fft (int size) : n (size)
{
    if (! isPowerOfTwo (size))
        throw std::invalid_argument ("Fft size must be a power of two");

    int bits = 0;
    while ((1 << bits) < n)
        ++bits;

    bitReversed.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        int reversed = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                reversed |= 1 << (bits - 1 - b);
        bitReversed[(size_t) i] = reversed;
    }

    twiddles.resize ((size_t) (n / 2));
    const double twoPi = 2.0 * M_PI;
    for (int k = 0; k < n / 2; ++k)
        twiddles[(size_t) k] = std::polar (1.0, -twoPi * k / n);
}

void Fft::forward (std::vector<std::complex<double>>& data) const { transform (data, false); }

void Fft::inverse (std::vector<std::complex<double>>& data) const
{
    transform (data, true);
    const double scale = 1.0 / n;
    for (auto& value : data)
        value *= scale;
}

void Fft::transform (std::vector<std::complex<double>>& data, bool inverse) const
{
    if ((int) data.size() != n)
        throw std::invalid_argument ("Fft data size mismatch");

    for (int i = 0; i < n; ++i)
        if (i < bitReversed[(size_t) i])
            std::swap (data[(size_t) i], data[(size_t) bitReversed[(size_t) i]]);

    for (int length = 2; length <= n; length <<= 1)
    {
        const int half = length / 2;
        const int step = n / length;
        for (int start = 0; start < n; start += length)
        {
            for (int k = 0; k < half; ++k)
            {
                auto w = twiddles[(size_t) (k * step)];
                if (inverse)
                    w = std::conj (w);
                const auto even = data[(size_t) (start + k)];
                const auto odd = data[(size_t) (start + k + half)] * w;
                data[(size_t) (start + k)] = even + odd;
                data[(size_t) (start + k + half)] = even - odd;
            }
        }
    }
}

} // namespace shizuku
