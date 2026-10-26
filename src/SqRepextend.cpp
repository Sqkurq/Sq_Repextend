// Sq_Repextend
// Repetile-style Expand Right/Left/Down/Up controls that dilate the opaque area of a layer,
// filling the new pixels with the color of the nearest edge pixel (edge extend, as in Nuke's EdgeExtend).
//
// Distances are normalized per direction (dx / Expand Right etc.), so a pixel is filled when d <= 1.
// Algorithm (exact for all three shapes, O(pixels) per pass, independent of the expand amount):
//   pass 1 (blocks of input rows)    : nearest source to the left / right -> h(x, row), column-major
//   pass 2 (blocks of output columns): one-sided scans down and up the column
//       Circle  : lower envelope of parabolas h^2 + (dy/s)^2   (Felzenszwalb-Huttenlocher)
//       Diamond : running minimum of h - t/s                   (d = h + dy/s)
//       Square  : monotone stack + binary search               (d = max(h, dy/s))
//   then composites and writes the output pixels of that column block.

#include "SqRepextend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

static_assert(SQ_OUTFLAGS == (PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_I_EXPAND_BUFFER),
	"SQ_OUTFLAGS does not match GlobalSetup out_flags");
static_assert(SQ_OUTFLAGS2 == (PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER |
	PF_OutFlag2_SUPPORTS_THREADED_RENDERING),
	"SQ_OUTFLAGS2 does not match GlobalSetup out_flags2");
static_assert(SQ_PIPL_VERSION == PF_VERSION(SQ_MAJOR_VERSION, SQ_MINOR_VERSION, SQ_BUG_VERSION,
	PF_Stage_RELEASE, SQ_BUILD_VERSION),
	"SQ_PIPL_VERSION does not match my_version");

namespace {

const float kInf = std::numeric_limits<float>::infinity();
const float kFillEps = 1e-5f;
const A_long kBlock = 16;				// rows / columns per worker task
const int16_t kNoSource = INT16_MIN;
const A_long kMaxReach = 32000;			// horizontal offsets are stored as int16

struct Params {
	double	right, left, down, up;		// in (downsampled) pixels, Expand All already added
	A_long	shape;
	float	threshold;					// 0..1
	A_long	output;
	bool	opaqueFill;
	bool	expandBounds;
};

struct RenderData {
	Params	p;
	A_long	reachR, reachL, reachD, reachU;
	PF_LRect inRect;					// layer-space rect of the checked-out input
	PF_LRect outRect;					// layer-space rect of the output buffer
};

void DeleteRenderData(void *data)
{
	delete reinterpret_cast<RenderData *>(data);
}

bool IsEmpty(const PF_LRect &r)
{
	return r.right <= r.left || r.bottom <= r.top;
}

PF_LRect Intersect(const PF_LRect &a, const PF_LRect &b)
{
	PF_LRect r;
	r.left		= std::max(a.left, b.left);
	r.top		= std::max(a.top, b.top);
	r.right		= std::min(a.right, b.right);
	r.bottom	= std::min(a.bottom, b.bottom);
	if (IsEmpty(r)) {
		r.left = r.top = r.right = r.bottom = 0;
	}
	return r;
}

PF_LRect Grow(const PF_LRect &r, A_long left, A_long top, A_long right, A_long bottom)
{
	if (IsEmpty(r)) {
		return r;
	}
	PF_LRect g = r;
	g.left		-= left;
	g.top		-= top;
	g.right		+= right;
	g.bottom	+= bottom;
	return g;
}

A_long Reach(double amount)
{
	if (!(amount >= 1.0)) return 0;		// also catches NaN
	return static_cast<A_long>(std::min(std::floor(amount + 1e-6), static_cast<double>(kMaxReach)));
}

// ---------------------------------------------------------------------------------------------
// Pixel traits

template <typename P> struct PixelTraits;
template <> struct PixelTraits<PF_Pixel8>		{ static constexpr float kMax = PF_MAX_CHAN8; };
template <> struct PixelTraits<PF_Pixel16>		{ static constexpr float kMax = PF_MAX_CHAN16; };
template <> struct PixelTraits<PF_PixelFloat>	{ static constexpr float kMax = 1.0f; };

template <typename P>
inline float ToFloat(decltype(P::alpha) v)
{
	return static_cast<float>(v) / PixelTraits<P>::kMax;
}

template <typename P>
inline decltype(P::alpha) FromFloat(float v)
{
	using C = decltype(P::alpha);
	if (std::is_floating_point<C>::value) {
		return static_cast<C>(v);
	}
	const float s = v * PixelTraits<P>::kMax + 0.5f;
	return static_cast<C>(std::min(std::max(s, 0.0f), PixelTraits<P>::kMax));
}

// Upstream float effects can hand us NaN / Inf / negative alpha. Treat such pixels as empty
// so they neither act as a source nor poison the composite.
template <typename P>
inline P Sanitize(const P &px)
{
	if (std::is_floating_point<decltype(P::alpha)>::value) {
		const float a = static_cast<float>(px.alpha);
		if (!(a > 0.0f) || !std::isfinite(a) || !std::isfinite(static_cast<float>(px.red)) ||
			!std::isfinite(static_cast<float>(px.green)) || !std::isfinite(static_cast<float>(px.blue))) {
			P z;
			std::memset(&z, 0, sizeof(z));
			return z;
		}
	}
	return px;
}

template <typename P>
inline P *Row(const PF_EffectWorld *w, A_long y)
{
	return reinterpret_cast<P *>(reinterpret_cast<char *>(w->data) + static_cast<ptrdiff_t>(y) * w->rowbytes);
}

// ---------------------------------------------------------------------------------------------
// One-sided column scans.
// Rows are mapped to t = dir * row so every scan runs with increasing t; queries at q = dir * yi
// only see rows with t <= q (dir = +1: sources above, extending down; dir = -1: sources below).
// Each call lowers bestD / bestR where it finds something strictly closer.

struct ScanBuffers {
	std::vector<double>	t, f, z;
	std::vector<A_long>	r;
};

template <A_long Shape>
void ScanColumn(const float *h, A_long Hi, A_long H, A_long offY, int dir, double scale,
	ScanBuffers &buf, float *bestD, A_long *bestR)
{
	const A_long yBegin	= dir > 0 ? 0 : H - 1;
	const A_long yEnd	= dir > 0 ? H : -1;
	A_long next			= dir > 0 ? 0 : Hi - 1;	// next input row to insert
	const double invS	= 1.0 / scale;

	auto pending = [&](A_long yi) {
		return dir > 0 ? (next < Hi && next <= yi) : (next >= 0 && next >= yi);
	};

	if (Shape == SHAPE_CIRCLE) {
		// lower envelope of f_i(q) = h_i^2 + a (q - t_i)^2
		const double a = invS * invS;
		A_long n = 0, k = 0;
		for (A_long y = yBegin; y != yEnd; y += dir) {
			const A_long yi = y + offY;
			for (; pending(yi); next += dir) {
				const float hv = h[next];
				if (hv == kInf) continue;
				const double tq = static_cast<double>(dir) * next;
				const double fq = static_cast<double>(hv) * hv;
				double s = -std::numeric_limits<double>::infinity();
				while (n > 0) {
					const A_long j = n - 1;
					s = ((fq + a * tq * tq) - (buf.f[j] + a * buf.t[j] * buf.t[j])) / (2.0 * a * (tq - buf.t[j]));
					if (s <= buf.z[j]) {
						--n;
					} else {
						break;
					}
				}
				buf.t[n] = tq;
				buf.f[n] = fq;
				buf.r[n] = next;
				buf.z[n] = n == 0 ? -std::numeric_limits<double>::infinity() : s;
				++n;
			}
			if (n == 0) continue;
			const double q = static_cast<double>(dir) * yi;
			if (k > n - 1) k = n - 1;
			while (k + 1 < n && buf.z[k + 1] < q) ++k;
			const double dq = q - buf.t[k];
			const float d = static_cast<float>(buf.f[k] + a * dq * dq);
			if (d < bestD[y]) {
				bestD[y] = d;
				bestR[y] = buf.r[k];
			}
		}
	} else if (Shape == SHAPE_DIAMOND) {
		// d = h_i + (q - t_i) / s  ->  running min of (h_i - t_i / s)
		double m = std::numeric_limits<double>::infinity();
		A_long mr = -1;
		for (A_long y = yBegin; y != yEnd; y += dir) {
			const A_long yi = y + offY;
			for (; pending(yi); next += dir) {
				const float hv = h[next];
				if (hv == kInf) continue;
				const double v = hv - static_cast<double>(dir) * next * invS;
				if (v <= m) {			// <= : on ties prefer the nearer row
					m = v;
					mr = next;
				}
			}
			if (mr < 0) continue;
			const float d = static_cast<float>(m + static_cast<double>(dir) * yi * invS);
			if (d < bestD[y]) {
				bestD[y] = d;
				bestR[y] = mr;
			}
		}
	} else {
		// d = max(h_i, (q - t_i) / s). A farther row with h >= a nearer row's h is never better,
		// so the stack holds rows far -> near with h strictly increasing (g decreasing).
		A_long head = 0, n = 0;
		for (A_long y = yBegin; y != yEnd; y += dir) {
			const A_long yi = y + offY;
			for (; pending(yi); next += dir) {
				const float hv = h[next];
				if (hv == kInf) continue;
				while (n > head && buf.f[n - 1] >= hv) --n;
				buf.t[n] = static_cast<double>(dir) * next;
				buf.f[n] = hv;
				buf.r[n] = next;
				++n;
			}
			const double q = static_cast<double>(dir) * yi;
			// rows beyond the reach stay out of reach as q grows
			while (head < n && (q - buf.t[head]) * invS > 1.0 + kFillEps) ++head;
			if (head >= n) continue;
			// first index where h >= g
			A_long lo = head, hi = n;
			while (lo < hi) {
				const A_long mid = (lo + hi) >> 1;
				if (buf.f[mid] >= (q - buf.t[mid]) * invS) hi = mid; else lo = mid + 1;
			}
			float d = kInf;
			A_long r = -1;
			if (lo < n) {
				d = static_cast<float>(buf.f[lo]);
				r = buf.r[lo];
			}
			if (lo > head) {
				const float g = static_cast<float>((q - buf.t[lo - 1]) * invS);
				if (g < d) {
					d = g;
					r = buf.r[lo - 1];
				}
			}
			if (d < bestD[y]) {
				bestD[y] = d;
				bestR[y] = r;
			}
		}
	}
}

// Same row only (reach 0 in that direction).
void ScanSameRow(const float *h, A_long Hi, A_long H, A_long offY, A_long shape, float *bestD, A_long *bestR)
{
	for (A_long y = 0; y < H; ++y) {
		const A_long yi = y + offY;
		if (yi < 0 || yi >= Hi) continue;
		const float hv = h[yi];
		if (hv == kInf) continue;
		const float d = shape == SHAPE_CIRCLE ? hv * hv : hv;
		if (d < bestD[y]) {
			bestD[y] = d;
			bestR[y] = yi;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Render context shared by the passes

template <typename P>
struct Context {
	const RenderData		*rd;
	const PF_EffectWorld	*in;		// may be null (empty input)
	PF_EffectWorld			*out;
	A_long	W, H;						// output size
	A_long	Wi, Hi;						// input size
	A_long	offX, offY;					// output coord + off = input coord
	A_long	colBegin, colEnd;			// output columns that can see any input column
	double	invR, invL;					// 1 / Expand Right, 1 / Expand Left (0 when unused)
	// (colEnd - colBegin) * Hi, column-major: signed offset to the nearest horizontal source,
	// xi - sourceColumn (> 0: source on the left), or kNoSource. 2 bytes per cell keeps this pass
	// memory-bandwidth friendly; the normalized distance is rebuilt per column in pass 2.
	std::unique_ptr<int16_t[]>	hoff;

	bool IsSource(const P &px) const
	{
		const float a = ToFloat<P>(px.alpha);
		return a > 0.0f && a >= rd->p.threshold - 1e-6f;
	}
};

template <typename P>
PF_Err HorizontalPass(void *refcon, A_long /*thread*/, A_long block, A_long /*iterations*/)
{
	Context<P> &c = *reinterpret_cast<Context<P> *>(refcon);
	const RenderData &rd = *c.rd;
	const A_long r0 = block * kBlock;
	const A_long r1 = std::min(c.Hi, r0 + kBlock);
	const A_long rows = r1 - r0;
	const A_long cw = c.colEnd - c.colBegin;

	std::vector<A_long> leftSrc(c.Wi), rightSrc(c.Wi);
	std::vector<unsigned char> src(c.Wi);
	std::vector<int16_t> lo(static_cast<size_t>(rows) * cw);

	for (A_long rr = 0; rr < rows; ++rr) {
		const P *row = Row<P>(c.in, r0 + rr);
		int16_t *ho = &lo[static_cast<size_t>(rr) * cw];

		// nearest source column at or left of / at or right of each input column
		A_long last = -1;
		for (A_long x = 0; x < c.Wi; ++x) {
			src[x] = c.IsSource(Sanitize(row[x])) ? 1 : 0;
			if (src[x]) last = x;
			leftSrc[x] = last;
		}
		if (last < 0) {
			std::fill(ho, ho + cw, kNoSource);
			continue;
		}
		last = -1;
		for (A_long x = c.Wi - 1; x >= 0; --x) {
			if (src[x]) last = x;
			rightSrc[x] = last;
		}

		for (A_long x = c.colBegin; x < c.colEnd; ++x) {
			const A_long xi = x + c.offX;
			A_long left = -1, right = -1;
			if (xi >= c.Wi) {
				left = leftSrc[c.Wi - 1];
			} else if (xi < 0) {
				right = rightSrc[0];
			} else {
				left = leftSrc[xi];
				right = rightSrc[xi];
			}

			float best = kInf;
			int16_t off = kNoSource;
			if (left >= 0) {
				// source on the left, this pixel is reached by extending to the right
				const A_long d = xi - left;
				if (d == 0) {
					best = 0.0f;
					off = 0;
				} else if (d <= rd.reachR) {
					best = static_cast<float>(d * c.invR);
					off = static_cast<int16_t>(d);
				}
			}
			if (right >= 0 && best > 0.0f) {
				const A_long d = right - xi;
				if (d <= rd.reachL) {
					const float f = d == 0 ? 0.0f : static_cast<float>(d * c.invL);
					if (f < best) {
						best = f;
						off = static_cast<int16_t>(-d);
					}
				}
			}
			ho[x - c.colBegin] = off;
		}
	}

	// transpose into the column-major array (kBlock contiguous values per column)
	for (A_long x = 0; x < cw; ++x) {
		int16_t *dst = &c.hoff[static_cast<size_t>(x) * c.Hi + r0];
		for (A_long rr = 0; rr < rows; ++rr) {
			dst[rr] = lo[static_cast<size_t>(rr) * cw + x];
		}
	}
	return PF_Err_NONE;
}

template <typename P>
inline P Composite(const Params &p, const P &orig, bool origIsSource, bool filled, const P &ext)
{
	P result;
	switch (p.output) {
		case OUTPUT_EXTENSION_ONLY:
			if (origIsSource || !filled) {
				std::memset(&result, 0, sizeof(result));
			} else {
				result = ext;
			}
			return result;

		case OUTPUT_REPLACE:
			return (origIsSource || !filled) ? orig : ext;

		default: {	// OUTPUT_OVER: original (straight alpha) composited over the extension
			const float ao = ToFloat<P>(orig.alpha);
			if (!filled || ao >= 1.0f) {
				return orig;
			}
			const float ae = ToFloat<P>(ext.alpha);
			const float wo = ao;
			const float we = ae * (1.0f - ao);
			const float a = wo + we;
			if (a <= 0.0f) {
				std::memset(&result, 0, sizeof(result));
				return result;
			}
			const float inv = 1.0f / a;
			result.alpha	= FromFloat<P>(a);
			result.red		= FromFloat<P>((ToFloat<P>(orig.red)   * wo + ToFloat<P>(ext.red)   * we) * inv);
			result.green	= FromFloat<P>((ToFloat<P>(orig.green) * wo + ToFloat<P>(ext.green) * we) * inv);
			result.blue		= FromFloat<P>((ToFloat<P>(orig.blue)  * wo + ToFloat<P>(ext.blue)  * we) * inv);
			return result;
		}
	}
}

template <typename P>
PF_Err VerticalPass(void *refcon, A_long /*thread*/, A_long block, A_long /*iterations*/)
{
	Context<P> &c = *reinterpret_cast<Context<P> *>(refcon);
	const RenderData &rd = *c.rd;
	const Params &p = rd.p;
	const A_long x0 = block * kBlock;
	const A_long x1 = std::min(c.W, x0 + kBlock);
	const A_long cols = x1 - x0;

	std::vector<float> bestD(static_cast<size_t>(cols) * c.H, kInf);
	std::vector<A_long> bestR(static_cast<size_t>(cols) * c.H, -1);
	std::vector<A_long> bestC(static_cast<size_t>(cols) * c.H, -1);

	if (c.in) {
		ScanBuffers buf;
		buf.t.resize(c.Hi);
		buf.f.resize(c.Hi);
		buf.z.resize(c.Hi);
		buf.r.resize(c.Hi);
		std::vector<float> hcol(c.Hi);

		for (A_long x = std::max(x0, c.colBegin); x < std::min(x1, c.colEnd); ++x) {
			const int16_t *ho = &c.hoff[static_cast<size_t>(x - c.colBegin) * c.Hi];
			bool any = false;
			for (A_long r = 0; r < c.Hi; ++r) {
				const int16_t o = ho[r];
				if (o == kNoSource) {
					hcol[r] = kInf;
				} else {
					hcol[r] = o > 0 ? static_cast<float>(o * c.invR) : o < 0 ? static_cast<float>(-o * c.invL) : 0.0f;
					any = true;
				}
			}
			if (!any) continue;
			const float *h = hcol.data();
			float *bd = &bestD[static_cast<size_t>(x - x0) * c.H];
			A_long *br = &bestR[static_cast<size_t>(x - x0) * c.H];

			if (rd.reachD == 0 || rd.reachU == 0) {
				ScanSameRow(h, c.Hi, c.H, c.offY, p.shape, bd, br);
			}
			for (int dir = 1; dir >= -1; dir -= 2) {
				const A_long reach = dir > 0 ? rd.reachD : rd.reachU;
				const double scale = dir > 0 ? p.down : p.up;
				if (reach == 0) continue;
				switch (p.shape) {
					case SHAPE_SQUARE:	ScanColumn<SHAPE_SQUARE>(h, c.Hi, c.H, c.offY, dir, scale, buf, bd, br);	break;
					case SHAPE_DIAMOND:	ScanColumn<SHAPE_DIAMOND>(h, c.Hi, c.H, c.offY, dir, scale, buf, bd, br);	break;
					default:			ScanColumn<SHAPE_CIRCLE>(h, c.Hi, c.H, c.offY, dir, scale, buf, bd, br);	break;
				}
			}
			A_long *bc = &bestC[static_cast<size_t>(x - x0) * c.H];
			const A_long xi = x + c.offX;
			for (A_long y = 0; y < c.H; ++y) {
				if (br[y] >= 0) bc[y] = xi - ho[br[y]];
			}
		}
	}

	for (A_long y = 0; y < c.H; ++y) {
		const A_long yi = y + c.offY;
		const P *srcRow = (c.in && yi >= 0 && yi < c.Hi) ? Row<P>(c.in, yi) : nullptr;
		P *dst = Row<P>(c.out, y);

		for (A_long x = x0; x < x1; ++x) {
			const size_t k = static_cast<size_t>(x - x0) * c.H + y;
			const A_long xi = x + c.offX;
			P orig;
			if (srcRow && xi >= 0 && xi < c.Wi) {
				orig = Sanitize(srcRow[xi]);
			} else {
				std::memset(&orig, 0, sizeof(orig));
			}
			// fully opaque originals pass through unchanged in both keep-original modes
			if (p.output != OUTPUT_EXTENSION_ONLY && ToFloat<P>(orig.alpha) >= 1.0f &&
					(p.output == OUTPUT_OVER || c.IsSource(orig))) {
				dst[x] = orig;
				continue;
			}
			const bool filled = bestR[k] >= 0 && bestD[k] <= 1.0f + kFillEps;

			P ext;
			if (filled) {
				ext = Sanitize(Row<P>(c.in, bestR[k])[bestC[k]]);
				if (p.opaqueFill) {
					ext.alpha = FromFloat<P>(1.0f);
				}
			} else {
				std::memset(&ext, 0, sizeof(ext));
			}
			dst[x] = Composite(p, orig, c.IsSource(orig), filled, ext);
		}
	}
	return PF_Err_NONE;
}

// Runs fn over [0, count) on AE's worker threads, or serially if the suite is unavailable.
PF_Err Iterate(PF_InData *in_data, A_long count, void *refcon,
	PF_Err (*fn)(void *, A_long, A_long, A_long))
{
	if (count <= 0) return PF_Err_NONE;

	const PF_Iterate8Suite2 *iter = nullptr;
	SPBasicSuite *sp = in_data->pica_basicP;
	if (sp && sp->AcquireSuite(kPFIterate8Suite, kPFIterate8SuiteVersion2,
			reinterpret_cast<const void **>(&iter)) == kSPNoError && iter) {
		const PF_Err err = iter->iterate_generic(count, refcon, fn);
		sp->ReleaseSuite(kPFIterate8Suite, kPFIterate8SuiteVersion2);
		return err;
	}

	PF_Err err = PF_Err_NONE;
	for (A_long i = 0; i < count && !err; ++i) {
		err = fn(refcon, 0, i, count);
	}
	return err;
}

// Worker callbacks run on AE's threads behind a C interface: never let an exception cross it.
template <PF_Err (*Fn)(void *, A_long, A_long, A_long)>
PF_Err NoThrow(void *refcon, A_long thread, A_long i, A_long iterations)
{
	try {
		return Fn(refcon, thread, i, iterations);
	} catch (const std::bad_alloc &) {
		return PF_Err_OUT_OF_MEMORY;
	} catch (...) {
		return PF_Err_INTERNAL_STRUCT_DAMAGED;
	}
}

PF_Err CheckAbort(PF_InData *in_data)
{
	return (in_data->inter.abort) ? PF_ABORT(in_data) : PF_Err_NONE;
}

template <typename P>
PF_Err RenderDepth(PF_InData *in_data, const RenderData &rd, const PF_EffectWorld *in, PF_EffectWorld *out)
{
	Context<P> c;
	c.rd	= &rd;
	c.in	= (in && in->data && in->width > 0 && in->height > 0) ? in : nullptr;
	c.out	= out;
	c.W		= out->width;
	c.H		= out->height;
	c.Wi	= c.in ? in->width : 0;
	c.Hi	= c.in ? in->height : 0;
	c.offX	= rd.outRect.left - rd.inRect.left;
	c.offY	= rd.outRect.top - rd.inRect.top;
	c.invR	= rd.reachR > 0 ? 1.0 / rd.p.right : 0.0;
	c.invL	= rd.reachL > 0 ? 1.0 / rd.p.left : 0.0;

	// output columns whose horizontal reach can touch the input at all:
	// left of the input only via Expand Left, right of it only via Expand Right
	c.colBegin = std::max<A_long>(0, -c.offX - rd.reachL);
	c.colEnd = std::min<A_long>(c.W, c.Wi - c.offX + rd.reachR);
	if (c.colEnd < c.colBegin) c.colEnd = c.colBegin;
	if (!c.in) c.colBegin = c.colEnd = 0;

	PF_Err err = PF_Err_NONE;
	try {
		const size_t cells = static_cast<size_t>(c.colEnd - c.colBegin) * c.Hi;
		if (cells > 0) {
			c.hoff.reset(new int16_t[cells]);
			err = Iterate(in_data, (c.Hi + kBlock - 1) / kBlock, &c, NoThrow<HorizontalPass<P>>);
			if (!err) err = CheckAbort(in_data);
		}
		if (!err) {
			err = Iterate(in_data, (c.W + kBlock - 1) / kBlock, &c, NoThrow<VerticalPass<P>>);
		}
	} catch (const std::bad_alloc &) {
		err = PF_Err_OUT_OF_MEMORY;
	}
	return err;
}

// ---------------------------------------------------------------------------------------------
// Commands

PF_Err About(PF_InData *in_data, PF_OutData *out_data)
{
	PF_SPRINTF(out_data->return_msg,
		"%s v%d.%d\r"
		"%s\r"
		"Edge extend with Repetile-style controls.\r"
		"Extends the alpha area with the color of the nearest edge pixel.",
		SQ_NAME, SQ_MAJOR_VERSION, SQ_MINOR_VERSION, SQ_LICENSE);
	return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData *in_data, PF_OutData *out_data)
{
	out_data->my_version = PF_VERSION(SQ_MAJOR_VERSION, SQ_MINOR_VERSION, SQ_BUG_VERSION,
		PF_Stage_RELEASE, SQ_BUILD_VERSION);
	out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_I_EXPAND_BUFFER;
	out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER |
		PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
	return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData *in_data, PF_OutData *out_data)
{
	PF_Err err = PF_Err_NONE;
	PF_ParamDef def;

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Expand Right", 0, 10000, 0, 200, 0, 1, PF_ValueDisplayFlag_NONE, 0, SQ_EXPAND_RIGHT);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Expand Left", 0, 10000, 0, 200, 0, 1, PF_ValueDisplayFlag_NONE, 0, SQ_EXPAND_LEFT);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Expand Down", 0, 10000, 0, 200, 0, 1, PF_ValueDisplayFlag_NONE, 0, SQ_EXPAND_DOWN);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Expand Up", 0, 10000, 0, 200, 0, 1, PF_ValueDisplayFlag_NONE, 0, SQ_EXPAND_UP);
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Expand All", 0, 10000, 0, 50, 0, 1, PF_ValueDisplayFlag_NONE, 0, SQ_EXPAND_ALL);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Shape", 3, SHAPE_CIRCLE, "Square|Circle|Diamond", SQ_SHAPE);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Alpha Threshold", 0, 100, 0, 100, 50, 1, PF_ValueDisplayFlag_PERCENT, 0, SQ_THRESHOLD);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Output", 3, OUTPUT_OVER,
		"Original Over Extension|Replace Below Threshold|Extension Only", SQ_OUTPUT);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOX("Opaque Fill", "On", TRUE, 0, SQ_OPAQUE_FILL);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOX("Expand Layer Bounds", "On", TRUE, 0, SQ_EXPAND_BOUNDS);

	// License line shown in the Effect Controls panel (an empty, collapsed group)
	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC(SQ_LICENSE, SQ_LICENSE_START);
	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(SQ_LICENSE_END);

	out_data->num_params = SQ_NUM_PARAMS;
	return err;
}

PF_Err ReadParams(PF_InData *in_data, Params *p)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	PF_ParamDef v[SQ_NUM_PARAMS];

	for (A_long i = SQ_EXPAND_RIGHT; i <= SQ_EXPAND_BOUNDS && !err; ++i) {
		AEFX_CLR_STRUCT(v[i]);
		err = PF_CHECKOUT_PARAM(in_data, i, in_data->current_time, in_data->time_step,
			in_data->time_scale, &v[i]);
		if (err) {
			for (A_long j = SQ_EXPAND_RIGHT; j < i; ++j) {
				ERR2(PF_CHECKIN_PARAM(in_data, &v[j]));
			}
			return err;
		}
	}

	const double all = v[SQ_EXPAND_ALL].u.fs_d.value;
	const double dsx = in_data->downsample_x.den
		? static_cast<double>(in_data->downsample_x.num) / in_data->downsample_x.den : 1.0;
	const double dsy = in_data->downsample_y.den
		? static_cast<double>(in_data->downsample_y.num) / in_data->downsample_y.den : 1.0;

	p->right		= std::max(0.0, (v[SQ_EXPAND_RIGHT].u.fs_d.value + all) * dsx);
	p->left			= std::max(0.0, (v[SQ_EXPAND_LEFT].u.fs_d.value + all) * dsx);
	p->down			= std::max(0.0, (v[SQ_EXPAND_DOWN].u.fs_d.value + all) * dsy);
	p->up			= std::max(0.0, (v[SQ_EXPAND_UP].u.fs_d.value + all) * dsy);
	p->shape		= v[SQ_SHAPE].u.pd.value;
	p->threshold	= static_cast<float>(v[SQ_THRESHOLD].u.fs_d.value / 100.0);
	p->output		= v[SQ_OUTPUT].u.pd.value;
	p->opaqueFill	= v[SQ_OPAQUE_FILL].u.bd.value != 0;
	p->expandBounds	= v[SQ_EXPAND_BOUNDS].u.bd.value != 0;

	for (A_long i = SQ_EXPAND_RIGHT; i <= SQ_EXPAND_BOUNDS; ++i) {
		ERR2(PF_CHECKIN_PARAM(in_data, &v[i]));
	}
	return err2;
}

PF_Err PreRender(PF_InData *in_data, PF_OutData * /*out_data*/, PF_PreRenderExtra *extra)
{
	PF_Err err = PF_Err_NONE;

	std::unique_ptr<RenderData> rd(new (std::nothrow) RenderData());
	if (!rd) return PF_Err_OUT_OF_MEMORY;

	err = ReadParams(in_data, &rd->p);
	if (err) return err;
	rd->reachR = Reach(rd->p.right);
	rd->reachL = Reach(rd->p.left);
	rd->reachD = Reach(rd->p.down);
	rd->reachU = Reach(rd->p.up);

	// An output pixel at x is fed by sources in [x - reachR, x + reachL] (same for y with D / U).
	PF_RenderRequest req = extra->input->output_request;
	if (!IsEmpty(req.rect)) {
		req.rect = Grow(req.rect, rd->reachR, rd->reachD, rd->reachL, rd->reachU);
	}

	PF_CheckoutResult in_result;
	AEFX_CLR_STRUCT(in_result);
	err = extra->cb->checkout_layer(in_data->effect_ref, SQ_INPUT, SQ_INPUT, &req,
		in_data->current_time, in_data->time_step, in_data->time_scale, &in_result);
	if (err) return err;

	// Sources spread by reachL to the left, reachR to the right, etc.
	const PF_LRect maxRect = rd->p.expandBounds
		? Grow(in_result.max_result_rect, rd->reachL, rd->reachU, rd->reachR, rd->reachD)
		: in_result.max_result_rect;
	const PF_LRect grown = Grow(in_result.result_rect, rd->reachL, rd->reachU, rd->reachR, rd->reachD);
	const PF_LRect result = Intersect(Intersect(grown, maxRect), extra->input->output_request.rect);

	rd->inRect = in_result.result_rect;
	rd->outRect = result;

	extra->output->result_rect = result;
	extra->output->max_result_rect = maxRect;
	extra->output->solid = FALSE;
	extra->output->pre_render_data = rd.release();
	extra->output->delete_pre_render_data_func = DeleteRenderData;
	return err;
}

PF_Err SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	const RenderData *rd = reinterpret_cast<const RenderData *>(extra->input->pre_render_data);
	if (!rd) return PF_Err_BAD_CALLBACK_PARAM;

	PF_EffectWorld *inW = nullptr, *outW = nullptr;
	err = extra->cb->checkout_layer_pixels(in_data->effect_ref, SQ_INPUT, &inW);
	const bool inputCheckedOut = !err;
	ERR(extra->cb->checkout_output(in_data->effect_ref, &outW));

	if (!err && outW && outW->data && outW->width > 0 && outW->height > 0) {
		PF_PixelFormat fmt = PF_PixelFormat_INVALID;
		const PF_WorldSuite2 *ws = nullptr;
		SPBasicSuite *sp = in_data->pica_basicP;
		if (sp->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2,
				reinterpret_cast<const void **>(&ws)) == kSPNoError && ws) {
			err = ws->PF_GetPixelFormat(outW, &fmt);
			sp->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
		} else {
			err = PF_Err_BAD_CALLBACK_PARAM;
		}

		if (!err) {
			switch (fmt) {
				case PF_PixelFormat_ARGB128:
					err = RenderDepth<PF_PixelFloat>(in_data, *rd, inW, outW);
					break;
				case PF_PixelFormat_ARGB64:
					err = RenderDepth<PF_Pixel16>(in_data, *rd, inW, outW);
					break;
				case PF_PixelFormat_ARGB32:
					err = RenderDepth<PF_Pixel8>(in_data, *rd, inW, outW);
					break;
				default:
					err = PF_Err_BAD_CALLBACK_PARAM;
					break;
			}
		}
	}

	if (inputCheckedOut) {
		ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, SQ_INPUT));
	}
	(void)out_data;
	return err ? err : err2;
}

} // namespace

extern "C" DllExport
PF_Err PluginDataEntryFunction2(
	PF_PluginDataPtr	inPtr,
	PF_PluginDataCB2	inPluginDataCallBackPtr,
	SPBasicSuite		*inSPBasicSuitePtr,
	const char			*inHostName,
	const char			*inHostVersion)
{
	PF_Err result = PF_Err_INVALID_CALLBACK;
	result = PF_REGISTER_EFFECT_EXT2(
		inPtr,
		inPluginDataCallBackPtr,
		SQ_NAME,
		SQ_MATCH_NAME,
		SQ_CATEGORY,
		AE_RESERVED_INFO,
		"EffectMain",
		SQ_SUPPORT_URL);
	return result;
}

PF_Err EffectMain(
	PF_Cmd			cmd,
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		* /*params*/[],
	PF_LayerDef		* /*output*/,
	void			*extra)
{
	PF_Err err = PF_Err_NONE;
	try {
		switch (cmd) {
			case PF_Cmd_ABOUT:
				err = About(in_data, out_data);
				break;
			case PF_Cmd_GLOBAL_SETUP:
				err = GlobalSetup(in_data, out_data);
				break;
			case PF_Cmd_PARAMS_SETUP:
				err = ParamsSetup(in_data, out_data);
				break;
			case PF_Cmd_SMART_PRE_RENDER:
				err = PreRender(in_data, out_data, reinterpret_cast<PF_PreRenderExtra *>(extra));
				break;
			case PF_Cmd_SMART_RENDER:
				err = SmartRender(in_data, out_data, reinterpret_cast<PF_SmartRenderExtra *>(extra));
				break;
			default:
				break;
		}
	} catch (const std::bad_alloc &) {
		err = PF_Err_OUT_OF_MEMORY;
	} catch (...) {
		err = PF_Err_INTERNAL_STRUCT_DAMAGED;
	}
	return err;
}
