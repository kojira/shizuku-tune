#pragma once

#include <complex>
#include <vector>

namespace shizuku
{

// Iterative radix-2 complex FFT. Size must be a power of two.
class Fft
{
public:
    explicit Fft (int size);

    int size() const noexcept { return n; }

    // In-place forward transform (no scaling).
    void forward (std::vector<std::complex<double>>& data) const;

    // In-place inverse transform, scaled by 1/N.
    void inverse (std::vector<std::complex<double>>& data) const;

    static bool isPowerOfTwo (int value) noexcept { return value > 0 && (value & (value - 1)) == 0; }
    static int nextPowerOfTwo (int value) noexcept;

private:
    void transform (std::vector<std::complex<double>>& data, bool inverse) const;

    int n;
    std::vector<int> bitReversed;
    std::vector<std::complex<double>> twiddles; // exp(-2*pi*i*k/N), k < N/2
};

} // namespace shizuku
