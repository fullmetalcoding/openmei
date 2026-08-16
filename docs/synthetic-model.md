# The synthetic source: model, algorithm, and limits

`src/camera/backends/synthetic_backend.cpp` generates a DIMM spot pair from a
known Fried parameter so the measurement chain can be checked against ground
truth. This note records what is physically derived, what is a modelling
choice, and what the model cannot be used to validate.

**Provenance warning.** This is not an implementation of any single paper. It
was assembled from standard relations plus several phenomenological choices.
Where a coefficient comes from a specific source it is marked; where it is a
convenience it is marked as such. Anyone relying on this for published work
should check the primary literature rather than this file.

---

## 1. Seeing to Fried parameter

    epsilon = 0.98 * lambda / r0

The Kolmogorov long-exposure FWHM relation. Inverted to get `r0` from the
seeing the user sets. Standard; traceable to Fried and to Roddier's 1981
review of atmospheric turbulence in optical astronomy.

## 2. Differential motion variance — Sarazin & Roddier

    sigma_l^2 = 2 lambda^2 r0^(-5/3) [0.179 D^(-1/3) - 0.0968 d^(-1/3)]
    sigma_t^2 = 2 lambda^2 r0^(-5/3) [0.179 D^(-1/3) - 0.1450 d^(-1/3)]

with sigma in radians, D the sub-aperture diameter and d the baseline, both in
metres. Longitudinal is parallel to the line joining the sub-apertures;
transverse is perpendicular. Valid for D/d <= 0.5.

Source: Sarazin & Roddier (1990), the ESO DIMM paper. The coefficients were
checked against secondary sources during development; the primary paper was not
consulted directly.

Two known refinements, neither implemented in the generator:

- The mini-DIMM analysis argues the leading 0.179 should be 0.182, on the
  grounds that Sarazin & Roddier's expressions approximate Sasiela's result and
  hold for d >= 2D.
- Tokovinin (2002) gives more accurate response coefficients as a function of
  b = d/D, and notes that a DIMM measures Zernike tilt rather than image
  centroid — so G-tilt and Z-tilt coefficient sets differ. Reference point: at
  b = 2.5 the G-tilt values are K_l = 0.1956, K_t = 0.1270. The published K(b)
  fit is not reproduced in this codebase; `CoefficientModel::Manual` exists so
  the values can be entered from the paper.

**Circularity.** The generator uses these relations to produce motion and the
analysis inverts the same relations. The coefficients cancel exactly. This
validates centroiding, statistics, axis handling and the inversion arithmetic —
it cannot validate whether the coefficients themselves are correct. Only a
second instrument or the literature can settle that.

## 3. Temporal correlation — the largest liberty taken

Differential motion evolves as an Ornstein-Uhlenbeck process:

    x(t+dt) = a x(t) + sigma sqrt(1 - a^2) N(0,1),   a = exp(-dt/tau)

stationary with variance sigma^2 and correlation time tau, exposed as
`coherenceTimeMs`.

**This is a modelling convenience, not atmospheric physics.** An OU process has
a Lorentzian power spectrum: flat below the corner frequency, falling as f^-2
above it. The real temporal spectrum of atmospheric tilt under frozen flow goes
roughly as f^(-2/3) at low frequency and f^(-11/3) at high frequency.

So relative to reality this model has too little low-frequency power and too
much high-frequency power. Since a finite exposure preferentially averages away
high frequencies, the consequence is directional: **the simulator probably
overstates exposure-time bias.**

The coherence time is also a free parameter here rather than derived. Physically
tau_0 ≈ 0.31 r0 / v_eff, so it is set by the same turbulence that sets r0
together with the wind profile — changing seeing without changing tau is not
something the atmosphere does.

## 4. Exposure-time integration

Each frame is built from N substeps spanning the exposure, with the atmosphere
advanced between them and the instantaneous PSF accumulated. The recorded image
is therefore the integral of a moving PSF: displaced by the flux-weighted mean
position and broadened by the spread, which is what a real exposure does.

Variance retention over a window T, exact for an OU process:

    R(T) = (2 tau / T) [1 - (tau/T)(1 - exp(-T/tau))]

Used only for the panel's prediction, not in the generator. Since the underlying
process is not really OU, this curve is indicative rather than authoritative.

The correction under test is Tokovinin's modified exponential extrapolation from
an interleaved pair:

    c1 = (eps1/eps2)^(3/4)
    eps0 = 0.5 (c1 eps1 + c1^(7/3) eps2)

Published bias magnitudes for calibration: Chilean site measurements alternating
10 and 20 ms give median seeing around 0.66-0.76" extrapolated to zero exposure
against 0.48-0.56" raw at 20 ms — roughly 25-30% low. Dome Fuji estimate their
1 ms exposures are biased under 3%.

## 5. Image formation

Each spot is an elliptical Gaussian:

    I(r) = (F / 2 pi s_maj s_min) exp(-[a^2/2s_maj^2 + b^2/2s_min^2])

with a and b the coordinates along and across the deviation axis. A real PSF is
an Airy pattern plus a seeing halo, better described by a Moffat profile; the
Gaussian is an approximation that understates the wings, which is where a
thresholded centroider gets much of its leverage.

**Wedge transmission.** Only the deviated spot passes through glass, so its flux
is scaled by `wedgeTransmission` — 0.92 for uncoated (two ~4% Fresnel surfaces),
~0.99 AR-coated.

**Chromatic streak.** A thin wedge deviates by delta = (n-1) alpha, and the
fractional spread across the passband is 1/V for Abbe number V. So the streak
length is delta/V, scaled by the fraction of the visible band transmitted. It is
folded into the major axis as the Gaussian equivalent of a uniform smear:

    s_maj = sqrt(sigma^2 + streak^2 / 12)

Limitation: this is **symmetric**. A real dispersion fan is weighted by the
source spectrum and the detector response, so it is asymmetric and would shift
the centroid, not merely broaden it. The model reproduces the precision loss
along the measurement axis but not that bias.

## 6. Scintillation

Per-aperture flux modulation, again OU, with a settable index and an
inter-aperture correlation coefficient. Entirely phenomenological. Real
scintillation follows from Fresnel propagation and depends on aperture size
relative to the Fresnel scale, zenith angle, and the Cn^2 profile; the
correlation between two sub-apertures depends on their separation relative to
that scale. None of that is modelled — the numbers are set by hand.

## 7. Detector

Standard CCD equation. Signal plus sky is clipped at full well, Poisson noise
applied, Gaussian read noise added, then converted to ADU:

    e_per_ADU = fullWell / (2^bits - 1) / 10^(gain/200)

The gain exponent follows the ZWO convention of 0.1 dB units. Two independent
ceilings are enforced — the well fills, and the ADC clips — because either one
flattens the core and biases the centroid toward better seeing.

Not modelled: dark current, fixed-pattern noise, amp glow, non-linearity near
saturation, rolling-shutter row timing.

## 8. Algorithm, per frame

    1. sigma_l, sigma_t from seeing via Sarazin & Roddier
    2. N = ceil(exposure / (tau/10)), clamped to [1, 64]
    3. for each substep:
         advance diff_l, diff_t, tracking, scintillation by dt (OU)
         place spot A at mid - sep/2 + diff/2
         place spot B at mid + sep/2 - diff/2
         accumulate both Gaussians at amplitude/N
         accumulate flux-weighted true positions
    4. publish ground truth (exposure-averaged, flux-weighted centroids)
    5. per pixel: clip to full well, Poisson, read noise, convert, quantise

Step 4 is what makes the centroider testable: the residual against those
positions is the centroider's error and nothing else.

## 9. What this can and cannot validate

Can:

- centroid accuracy, including pixel-phase bias (static + noise-free mode)
- centroid precision against the photon-noise floor
- variance accumulation, axis projection, rejection gates
- common-mode cancellation (vary tracking wander; r0 must not move)
- isotropy (rotate the baseline angle; r0 must not move)
- noise-bias subtraction (vary flux; r0 must not move)
- that the t/2t machinery recovers *the bias this model injects*

Cannot:

- the response coefficients — the generator and the analysis share them
- whether the exposure correction is right for the real atmosphere, since the
  temporal spectrum here is wrong in a known direction
- anything about real optics: aberrations, non-Gaussian PSF wings, focus drift
- absolute seeing accuracy at any level

The last point is the important one. Agreement with the synthetic source is
necessary but nowhere near sufficient. Absolute validation requires a second
instrument.