
// clang-format off: In his text, the writer sets up house. Just as he trundles papers, books, pencils, documents untidily from room to room, he creates the same disorder in his thoughts.
#include <Editor/AutoSync.h>

#include <Core/Vector.h>
#include <Core/Utils.h>

#include <System/Debug.h>

#include <Simfile/Segments.h>
#include <Simfile/Tempo.h>

#include <Editor/FindOnsets.h>
#include <Iir.h>

#include <algorithm>

namespace Vortex {

template<typename T>
struct Span {
	const T* data_;
	int size_;
	int stride_;

	Span() : data_(nullptr), size_(0), stride_(sizeof(T)) {}
	Span(const T* data, int size, int stride) : data_(data), size_(size), stride_(stride) {}
	Span(const T* data, int size, int start, int end) {
		start = clamp(start, 0, size);
		end = clamp(end, start, size);
		data_ = data + start;
		size_ = end - start;
		stride_ = sizeof(T);
	}
	Span(const T* data, int size) : Span(data, size, 0, size) {}
	Span(const Vector<T>& v) : Span(v.data(), v.size(), 0, v.size()) {}
	Span(const Vector<T>& v, int start) : Span(v.data(), v.size(), start, v.size()) {}
	Span(const Vector<T>& v, int start, int end) : Span(v.data(), v.size(), start, end) {}
	Span(const Span<T>& v, int start, int end) : Span(v.data_, v.size_, start, end) { stride_ = v.stride_; }

	const T* data() const { return data_; }
	const T& front() const { return operator[](0); }
	const T& back() const { return operator[](size_ - 1); }
	int size() const { return size_; }
	bool empty() const { return size_ == 0; }

	const T& operator[](int i) const { VortexAssert(i >= 0 && i < size_); return *(const T*)((const uint8_t*)data_ + i * stride_); }

	struct Iterator
	{
		const uint8_t* ptr;
		int stride;
		const T& operator*() const { return *(const T*)ptr; }
		Iterator& operator++() { ptr += stride; return *this; }
		bool operator!=(const Iterator& other) const { return ptr != other.ptr; }
	};

	Iterator begin() const { return { (const uint8_t*)data_, stride_ }; }
	Iterator end() const { return { (const uint8_t*)data_ + size_ * stride_, stride_ }; }
};

struct TimeOnset
{
	double time;
	double strength;
};

struct BpmResult
{
	double bpm;
	bool valid;
	double confidence;
	double oldBpmConfidence;
};

struct AlignedBeat
{
	int row;
	double score;
};

struct OnsetBpmEstimate
{
	double bpm;
	double confidence;
};

struct Anchor
{
	enum class Type { Bpm, Required, Onset };

	BpmChange seg;
	double time;
	double delta;
	int onset;
	Type type;

	static Anchor Bpm(int row, double time, double bpm, int onset)                  { return { { row, bpm }, time, 0.0,   onset, Type::Bpm }; }
	static Anchor Onset(int row, double time, double bpm, int onset, double delta)  { return { { row, bpm }, time, delta, onset, Type::Onset }; }
	static Anchor Required(int row, double time, double bpm, int onset)             { return { { row, bpm }, time, 0.0,   onset, Type::Required }; }
	static Anchor Refined(Anchor anchor, double refinedBpm)
	{
		Anchor result = { {anchor.seg.row, refinedBpm }, anchor.time, anchor.delta, anchor.onset, anchor.type };
		if (result.Onset()) result.type = Type::Bpm;
		return result;
	}

	bool Bpm() 	  	{ return type == Type::Bpm; }
	bool Onset()   	{ return type == Type::Onset; }
	bool Required()	{ return type == Type::Required; }

};

constexpr double MinSyncBPM = 45.0;
constexpr double MaxSyncBPM = 245.0;
constexpr double MaxTripletCorrectionBPM = 245.0 * (6.0 / 7.0);
constexpr double AnchorBpm = 60.0;
constexpr double BpmRegularityThreshold = 0.04;
constexpr double AnchorTimeThreshold = 0.006;
constexpr int RowsPerBeat = ROWS_PER_BEAT;
constexpr int RowsPer8th = RowsPerBeat / 2;
constexpr int RowsPer16th = RowsPerBeat / 4;
constexpr double Pi = 3.14159265358979323846;

constexpr double BeatsToTime(double beats, double bpm)
{
	return beats * 60.0 / bpm;
}

constexpr double TimeToBeats(double seconds, double bpm)
{
	return seconds * bpm / 60.0;
}

constexpr double BeatsPerMinute(double beats, double seconds)
{
	return beats * 60.0 / seconds;
}

constexpr int RoundToMultiple(int value, int multiple)
{
	return ((value + multiple / 2) / multiple) * multiple;
}

struct RowTime
{
	Span<BpmChange> bpms = {};
	int bpmIndex = 0;
	double bpmTime = 0.0;

	RowTime(Span<BpmChange> bpms, double offset) : bpms(bpms), bpmTime(-offset) {}
	RowTime(Span<Anchor> anchors) : bpms(anchors.size() ? &anchors[0].seg : nullptr, anchors.size(), sizeof(Anchor)), bpmTime(anchors.size() ? anchors[0].time : 0.0) {}

	double Time(int queriedRow)
	{
		if (bpms.empty() || queriedRow < 0)
			return bpmTime;

		VortexAssert((queriedRow == 0) || (queriedRow >= bpms[bpmIndex].row));

		int anchorRow = bpms[bpmIndex].row;
		double anchorBpm = bpms[bpmIndex].bpm;
		double anchorTime = bpmTime;
		int i = bpmIndex;
		for (; i + 1 < bpms.size() && bpms[i + 1].row <= queriedRow; ++i)
		{
			int rowDelta = bpms[i + 1].row - anchorRow;
			double beatDelta = double(rowDelta) / RowsPerBeat;
			anchorTime += BeatsToTime(beatDelta, anchorBpm);

			anchorRow = bpms[i + 1].row;
			anchorBpm = bpms[i + 1].bpm;
		}

		int rowDelta = queriedRow - anchorRow;
		double beatDelta = double(rowDelta) / RowsPerBeat;
		double result = anchorTime + BeatsToTime(beatDelta, anchorBpm);

		bpmIndex = i;
		bpmTime = anchorTime;
		return result;
	}

	int Row(double queriedTime)
	{
		if (bpms.empty())
			return 0;

		VortexAssert((bpmIndex == 0) || (queriedTime >= bpmTime));

		int anchorRow = bpms[bpmIndex].row;
		double anchorBpm = bpms[bpmIndex].bpm;
		double anchorTime = bpmTime;
		int i = bpmIndex;
		for (; i + 1 < bpms.size(); ++i)
		{
			int rowDelta = bpms[i + 1].row - anchorRow;
			double beatDelta = double(rowDelta) / RowsPerBeat;
			double nextTime = anchorTime + BeatsToTime(beatDelta, anchorBpm);

			if (nextTime > queriedTime)
				break;

			anchorTime = nextTime;
			anchorRow = bpms[i + 1].row;
			anchorBpm = bpms[i + 1].bpm;
		}

		double timeDelta = queriedTime - anchorTime;
		double beatDelta = TimeToBeats(timeDelta, anchorBpm);
		int result = anchorRow + int(beatDelta * RowsPerBeat + 0.5);

		bpmIndex = i;
		bpmTime = anchorTime;
		return result;
	}
};

struct Histogram
{
	static constexpr double MaxBpm = MaxSyncBPM * 2.0;
	static constexpr double MinBpm = MinSyncBPM * 0.5;
	static constexpr double MinInterval = BeatsToTime(1.0, MaxBpm);
	static constexpr double MaxInterval = BeatsToTime(1.0, MinBpm);
	static constexpr int BinsPerSecond = 180;
	static constexpr int Size = int((MaxInterval - MinInterval) * BinsPerSecond) + 1;
	static constexpr int HammingWindowRadius = 12;
	static constexpr int HammingWindowSize = HammingWindowRadius * 2 + 1;

	double bins[Size] = {};

	double Sample(double bpm) const
	{
		double result = 0.0;
		double interval = BeatsToTime(1.0, bpm);
		double bin = (interval - MinInterval) * BinsPerSecond;
		if (bin >= 0 && bin < Size - 1)
		{
			result = lerp(bins[int(bin)], bins[int(bin) + 1], bin - int(bin));
		}
		return result;
	}

	void Add(OnsetBpmEstimate estimate)
	{
		double interval = BeatsToTime(1.0, estimate.bpm);
		int bin = int((interval - MinInterval) * BinsPerSecond);
		VortexAssert(bin >= 0 && bin < Size);
		if (bin >= 0 && bin < Size)
		{
			bins[bin] += estimate.confidence;
		}
	}

	void Remove(OnsetBpmEstimate estimate)
	{
		double interval = BeatsToTime(1.0, estimate.bpm);
		int bin = int((interval - MinInterval) * BinsPerSecond);
		VortexAssert(bin >= 0 && bin < Size);
		if (bin >= 0 && bin < Size)
		{
			bins[bin] -= estimate.confidence;
		}
	}

	double Peak()
	{
		int result = 0;
		for (int i = 1; i < Size; ++i)
		{
			if (bins[i] > bins[result])
			{
				result = i;
			}
		}
		double interval = MinInterval + double(result) / BinsPerSecond;
		return BeatsPerMinute(1.0, interval);
	}

	Histogram Normalized()
	{
		Histogram result = *this;
		double sum = 0.0;
		for (int i = 0; i < Size; ++i)
		{
			sum += result.bins[i];
		}
		if (sum > 0.0)
		{
			for (int i = 0; i < Size; ++i)
			{
				result.bins[i] /= sum;
			}
		}
		return result;
	}

	static Histogram Build(Span<OnsetBpmEstimate> estimates)
	{
		Histogram raw = {};
		Histogram result = {};
		for (auto estimate : estimates)
		{
			raw.Add(estimate);
		}
		double window[HammingWindowSize];
		double t = Pi * 2.0 / (HammingWindowSize - 1);
		for (int w = 0; w < HammingWindowSize; ++w)
		{
			window[w] = 0.54 - 0.46 * cos(w * t);
		}
		double maximum = 0.0;
		for (int i = 0; i < Size; ++i) {
			int lo = clamp(i - HammingWindowRadius, 0, Size - 1);
			int hi = clamp(i + HammingWindowRadius, 0, Size - 1);
			int wOffset = (lo + HammingWindowRadius) - i;
			double sum = 0.0;
			for (int j = lo, w = wOffset; j <= hi; ++j, ++w)
			{
				sum += raw.bins[j] * window[w];
			}
			result.bins[i] = sum;
			maximum = max(maximum, sum);
		}
		if (maximum > estimates.size() / 10)
		{
			for (int i = 0; i < Size; ++i)
			{
				result.bins[i] = clamp(result.bins[i] - estimates.size() / 20, 0.0, double(INFINITY));
			}
		}

		return result;
	}
};

struct SlidingHistogram
{
	static constexpr int HammingWindowRadius = Histogram::HammingWindowRadius;
	static constexpr int HammingWindowSize = Histogram::HammingWindowSize;
	Histogram histogram;
	Vector<OnsetBpmEstimate> estimates;
	Vector<double> onsetToBeat;
	int windowStart = 0;
	int windowEnd = 0;
	int lookaheadBeats = 0;
	double window[Histogram::HammingWindowSize];

	static SlidingHistogram Build(const Vector<OnsetBpmEstimate>& estimates, Span<TimeOnset> onsets, int lookaheadBeats)
	{
		SlidingHistogram result = {};
		result.lookaheadBeats = lookaheadBeats;
		result.estimates = estimates;
		result.onsetToBeat = Vector<double>(onsets.size(), 0.0);
		for (int i = 1; i < onsets.size(); ++i)
		{
			double timeDelta = onsets[i].time - onsets[i - 1].time;
			double beatDelta = TimeToBeats(timeDelta, result.estimates[i].bpm);
			result.onsetToBeat[i] = result.onsetToBeat[i - 1] + beatDelta;
		}

		int lookaheadEnd = 0;
		for (int i = 0; i < onsets.size(); ++i)
		{
			if (result.onsetToBeat[i] >= lookaheadBeats)
			{
				lookaheadEnd = i;
				break;
			}
		}

		for (int i = 0; i < lookaheadEnd; ++i)
		{
			result.histogram.Add(result.estimates[i]);
		}

		double t = Pi * 2.0 / (HammingWindowSize - 1);
		for (int w = 0; w < HammingWindowSize; ++w)
		{
			result.window[w] = 0.54 - 0.46 * cos(w * t);
		}

		result.windowEnd = lookaheadEnd;
		return result;
	}

	void Advance(int onsetIndex)
	{
		double onsetBeat = onsetToBeat[onsetIndex];

		int end = windowEnd;
		for (; end < onsetToBeat.size(); ++end)
		{
			double beatDelta = onsetToBeat[end] - onsetBeat;
			if (beatDelta > lookaheadBeats)
				break;
			histogram.Add(estimates[end]);
		}

		for (int start = windowStart; start < onsetIndex; ++start)
		{
			histogram.Remove(estimates[start]);
		}

		windowEnd = end;
		windowStart = onsetIndex;
	}

	double Sample(double bpm) const
	{
		double result = 0.0;
		double centerInterval = BeatsToTime(1.0, bpm);
		constexpr double dInterval = 1.0 / Histogram::BinsPerSecond;
		for (int w = 0; w < Histogram::HammingWindowSize; ++w)
		{
			double interval = centerInterval + (w - Histogram::HammingWindowRadius) * dInterval;
			result += histogram.Sample(BeatsPerMinute(1.0, interval)) * window[w];
		}
		return result;
	}
};

static double ProbeTupletPulseTrain(Span<TimeOnset> onsets, double bpm)
{
	double omega = 2.0 * Pi * bpm / 60.0;
	double real[3] = { 0.0, 0.0, 0.0 };
	double imag[3] = { 0.0, 0.0, 0.0 };
	for (int i = 0; i < onsets.size(); ++i)
	{
		double t = onsets[i].time;
		double phase = omega * t;
		double strength = onsets[i].strength;
		real[0] += (strength * strength) * cos(1.0 * phase);
		imag[0] += (strength * strength) * sin(1.0 * phase);
		real[1] += (strength * strength) * cos(2.0 * phase);
		imag[1] += (strength * strength) * sin(2.0 * phase);
		real[2] += (strength * strength) * cos(4.0 * phase);
		imag[2] += (strength * strength) * sin(4.0 * phase);
	}

	double fourths    = (real[0] * real[0] + imag[0] * imag[0]) / onsets.size();
	double eights     = (real[1] * real[1] + imag[1] * imag[1]) / onsets.size();
	double sixteenths = (real[2] * real[2] + imag[2] * imag[2]) / onsets.size();
	return sqrt(fourths * eights * sixteenths);
}

static double FixTripletBPMs(Span<TimeOnset> onsets, double peakBpm)
{
	onsets = Span<TimeOnset>(onsets, 4*onsets.size()/10, 6*onsets.size()/10);
	double result = 1.0;
	double ratio = 3.0 / 2.0;
	double base = ProbeTupletPulseTrain(onsets, peakBpm);
	double major = ProbeTupletPulseTrain(onsets, peakBpm * ratio);
	if ((base * 4.0 < major || major >= 0.5) && peakBpm * ratio < MaxTripletCorrectionBPM)
	{
		result = ratio;
	}
	return result;
}

struct ClosestAliasBpmFlags
{
	enum
	{
		Default,
		NoTriplets
	};
};

static double ClosestAliasBpm(double bpm, double referenceBpm, int flags = ClosestAliasBpmFlags::Default)
{
	double currentBpm = 0.0;

	while (currentBpm != bpm)
	{
		currentBpm = bpm;

		// For octave relationships the superficially correct thing to do is compare ratios, not differences, because of
		// the logarithmic business going on. But if we do it this way then we systematically bias BPMs downwards. There
		// is a sweet spot for this algorithm about 60BPM, and, at higher BPMs, it works better to fit to every second
		// or fourth beat, rather than fitting to every beat.

		double distance = abs(bpm - referenceBpm);
		double doubleBpm = bpm * 2.0;
		double halfBpm = bpm * 0.5;

		double diffDouble = abs(doubleBpm - referenceBpm);
		double diffHalf = abs(halfBpm - referenceBpm);

		double tripletMinorBpm = bpm * 2.0 / 3.0;
		double tripletMajorBpm = bpm * 4.0 / 3.0;
		double diffMinor = abs(tripletMinorBpm - referenceBpm);
		double diffMajor = abs(tripletMajorBpm - referenceBpm);

		if (halfBpm >= MinSyncBPM && diffHalf < distance && diffHalf < diffDouble)
		{
			bpm = halfBpm;
			distance = diffHalf;
		}
		if (doubleBpm <= MaxSyncBPM && diffDouble < distance)
		{
			bpm = doubleBpm;
			distance = diffDouble;
		}

		if (!(flags & ClosestAliasBpmFlags::NoTriplets))
		{
			if (tripletMinorBpm >= MinSyncBPM && diffMinor < distance)
			{
				bpm = tripletMinorBpm;
				distance = diffMinor;
			}
			if (tripletMajorBpm <= MaxSyncBPM && diffMajor < distance)
			{
				bpm = tripletMajorBpm;
			}
		}
	}

	return bpm;
}

//
// Beat alignment measurements
//
// Evaluating how good a given BPM is at approximating a sequence of onsets
//

static double Measure(double onsetTime, double beatTime, double zerocross)
{
	// We score each onset based on its distance from beat positions using the mexican hat wavelet. It's just a curve
	// shaped like -_/\_-, centered on zero:
	//   - positive when onsets and the beat align closely,
	//   - negative when slightly off beat,
	//   - negative but tending towards zero further out
	//
	// zerocross defines how many seconds away from the beat it is that we transition from scoring onsets positively to
	// negatively.
	//
	// Math-wise the zero crossing occurs when the (1 - t^2) term is zero, so when t^2 = distance = 1.
	double distance = (onsetTime - beatTime) / zerocross;
	double tt = distance * distance;
	return (1.0 - tt) * exp(-0.5 * tt);
}

static double MeasureBeatAlignment(Span<TimeOnset> onsets, double interval, double alignmentTime)
{
	if (onsets.size() < 2) return 0.0;

	TimeOnset first = onsets.front();
	TimeOnset last = onsets.back();
	double score = 0.0;

	// Score every 16th against every onset. We do it this way because we have no idea how dense
	// or sparse the onsets are on the grid, and expect to be called with short spans of onsets.
	{
		double quarterInterval = interval / 4.0;
		double zerocross = sqrt(0.5) * quarterInterval / 2.0;
		int stepCount = 1 + int((last.time - alignmentTime) / quarterInterval);
		double windowRadius = 1.414 * interval;

		int windowStart = 0;
		int windowEnd = 0;

		for (int step = 0; step < stepCount; ++step)
		{
			double gridTime = alignmentTime + step * quarterInterval;
			double windowLo = gridTime - windowRadius;
			double windowHi = gridTime + windowRadius;

			while (windowStart < onsets.size() && onsets[windowStart].time < windowLo)
				windowStart++;
			while (windowEnd < onsets.size() && onsets[windowEnd].time <= windowHi)
				windowEnd++;

			for (int j = windowStart; j < windowEnd; ++j)
			{
				score += onsets[j].strength * Measure(onsets[j].time, gridTime, zerocross);
				score -= onsets[j].strength * Measure(onsets[j].time, gridTime + quarterInterval / 2, zerocross);
			}
		}
	}

	// And again for 12ths
	if (onsets.size() > 2)
	{
		double thirdInterval = interval / 3.0;
		double zerocross = sqrt(0.5) * thirdInterval / 2.0;
		int stepCount = 1 + int((last.time - alignmentTime) / thirdInterval);
		double windowRadius = 1.414 * interval;

		int windowStart = 0;
		int windowEnd = 0;

		double twelfthsScore = 0.0;
		for (int step = 0; step < stepCount; ++step)
		{
			double gridTime = alignmentTime + step * thirdInterval;
			double windowLo = gridTime - windowRadius;
			double windowHi = gridTime + windowRadius;

			while (windowStart < onsets.size() && onsets[windowStart].time < windowLo)
				windowStart++;
			while (windowEnd < onsets.size() && onsets[windowEnd].time <= windowHi)
				windowEnd++;

			for (int j = windowStart; j < windowEnd; ++j)
			{
				twelfthsScore += onsets[j].strength * Measure(onsets[j].time, gridTime, zerocross);
				twelfthsScore -= onsets[j].strength * Measure(onsets[j].time, gridTime + thirdInterval / 2, zerocross);
			}
		}

		if (twelfthsScore > score)
			score = twelfthsScore;
	}

	return max(0.0, score / onsets.size());
}

static double MeasureBeatAlignment(Span<TimeOnset> onsets, double interval)
{
	return MeasureBeatAlignment(onsets, interval, !onsets.empty() ? onsets.front().time : 0.0);
}

static OnsetBpmEstimate EstimateLocalBpm(Span<TimeOnset> onsets, int onsetIndex, double referenceBpm, SlidingHistogram* histogram = nullptr)
{
	// Turns clusters of onsets into a BPM. We gather up all onsets within 1 beat, at the referenceBpm, of
	// the onset at onsetIndex. For every onset there we want to ask, if the distance between this onset and
	// the central onset was 1 beat, how well does that BPM align with the rest of these onsets?

	// referenceBpm is supposed to be whatever we think the BPM is likely to be but it might be a bad choice, and the new
	// bpm will give a new window of onsets to test. If we can get a higher confidence bpm with that new window, we should
	// use it. Strong expectation here that most BPMs will break out of the loop on the second iteration

	OnsetBpmEstimate result = { referenceBpm, 0.0 };
	Span<TimeOnset> window;
	for (size_t iters = 0; iters < 10; ++iters)
	{
		double currentEstimate = result.bpm;
		double windowDuration = BeatsToTime(1.0, currentEstimate);

		double onsetTime = onsets[onsetIndex].time;
		int endIndex = onsetIndex + 1;
		for (; endIndex < onsets.size(); ++endIndex)
		{
			if ((onsets[endIndex].time - onsetTime) > windowDuration)
				break;
		}
		int startIndex = onsetIndex - 1;
		for (; startIndex >= 0; --startIndex)
		{
			if (onsetTime - onsets[startIndex].time > windowDuration)
				break;
		}

		// Don't need to rescore if the onsets in the window haven't changed after refining the BPM
		if ((endIndex - startIndex) == window.size())
		{
			break;
		}

		window = Span<TimeOnset>(onsets, startIndex, endIndex);

		double referenceInterval = BeatsToTime(1.0, currentEstimate);
		double bestConfidence = MeasureBeatAlignment(window, referenceInterval);
		if (histogram) bestConfidence *= histogram->Sample(currentEstimate);
		double bestBpm = currentEstimate;
		for (auto onset : window)
		{
			if (onset.time == onsetTime) continue;
			double timeDelta = fabs(onset.time - onsetTime);
			double candidateBpm = BeatsPerMinute(1.0, timeDelta);
			double interval = timeDelta;
			double confidence = MeasureBeatAlignment(window, interval);
			if (histogram) confidence *= histogram->Sample(candidateBpm);
			if (confidence > bestConfidence)
			{
				bestBpm = BeatsPerMinute(1.0, timeDelta);
				bestConfidence = confidence;
			}
		}

		while (bestBpm < MinSyncBPM) bestBpm *= 2.0;
		while (bestBpm > MaxSyncBPM) bestBpm /= 2.0;

		if (bestConfidence <= result.confidence)
		{
			break;
		}

		result = { bestBpm, bestConfidence };
	}

	return result;
}

static AlignedBeat MeasureBpmPlacement(Span<TimeOnset> onsets, Anchor anchor, double endTime, double testBpm, double testTime = 0)
{
	AlignedBeat result = {};

	double timeDelta = endTime - anchor.time;
	VortexAssert(timeDelta > 0);

	int rowDelta = int(TimeToBeats(timeDelta, testBpm) * RowsPerBeat + 0.5);
	int baseRow = RoundToMultiple(anchor.seg.row + rowDelta, RowsPerBeat);

	// For large steps, nearby beats may result in very similar BPMs, so try multiple
	int candidateRows[] = {
		 baseRow - RowsPerBeat,
		 baseRow,
		 baseRow + RowsPerBeat
	};
	for (int row : candidateRows)
	{
		if (row <= anchor.seg.row) continue;

		int quantizedRowDelta = row - anchor.seg.row;
		double beatDelta = double(quantizedRowDelta) / RowsPerBeat;
		double inducedBpm = ClosestAliasBpm(BeatsPerMinute(beatDelta, timeDelta), testBpm);

		// We reject BPMs that aren't close to the estimate. Generally the estimate is pretty good,
		// so if this BPM isn't close to it's almost certainly bad. But we generate the estimates
		// using the same BeatAlignment idea, so bad bpms will score low. We don't want to rely
		// on that as BPMs are assigned greedily, and the best from a set of bad options will be chosen.
		// We can reject onsets we know are too bad to even be part of the pool here.
		bool bpmWithinThreshold = abs(testBpm / inducedBpm - 1.0) < BpmRegularityThreshold;

		// Also check against anchor BPM using testTime. This is for testing bpms without a corresponding onset; in this case we might have snapped
		// onto an onset for the purposes of sync but not measuring, which requires this extra test
		if (testTime > 0)
		{
			double testTimeDelta = testTime - anchor.time;
			double synthBeatDelta = double(row - anchor.seg.row) / RowsPerBeat;
			double synthInducedBpm = ClosestAliasBpm(BeatsPerMinute(synthBeatDelta, testTimeDelta), anchor.seg.bpm);
			double mpbError = abs(anchor.seg.bpm / synthInducedBpm - 1.0);
			if (mpbError > BpmRegularityThreshold)
			{
				bpmWithinThreshold = false;
			}
		}

		if (bpmWithinThreshold && inducedBpm > MinSyncBPM && inducedBpm < MaxSyncBPM)
		{
			double interval = BeatsToTime(1.0, inducedBpm);
			double score = MeasureBeatAlignment(onsets, interval, anchor.time);

			if (score > result.score)
			{
				result = { row, score };
			}
		}
	}

	return result;
}

static double SnapToNearestOnset(Span<TimeOnset> onsets, int startIndex, double targetTime, double beatInterval)
{
	double snapped = targetTime;
	double closest = beatInterval / 2.0;
	double startTime = targetTime - closest;

	for (int j = startIndex - 1; j >= 0; j--)
	{
		double time = onsets[j].time;
		if (time < startTime) break;
		double distance = abs(targetTime - time);
		if (distance < closest)
		{
			closest = distance;
			snapped = time;
		}
	}

	double endTime = targetTime + closest;
	for (int j = startIndex; j < onsets.size(); j++)
	{
		double time = onsets[j].time;
		if (time > endTime) break;
		double distance = abs(targetTime - time);
		if (distance < closest)
		{
			closest = distance;
			snapped = time;
		}
	}

	return snapped;
}

struct FitOptions
{
	enum
	{
		Default         = 0,
		AlignToEnd      = 1 << 0,
		AliasToStartBpm = 1 << 1
	};
};

static void Align(Vector<Anchor>& anchors, Anchor startReq, Anchor endReq)
{
	VortexAssert(anchors.size() > 0);
	VortexAssert(anchors[0].seg.row == startReq.seg.row);

	double toleranceMultiplier = 4.0;

	int remainingAnchors = anchors.size() - 1;

	// This may fail to align the end requirement cleanly. In that case all the anchors we just computed get discarded
	bool aligned = false;
	int adjustedEndReqRow = endReq.seg.row;
	while (!aligned && remainingAnchors-- >= 0)
	{
		// Try to align endReq by inserting a new anchor on a beat before endReq.time. Start from the last beat
		// before endReq and work backwards. If we reach the row of anchors.back(), try updating its value instead
		// of inserting. If that fails, pop the anchor and retry with more room

		Anchor anchor = anchors.back();

		double timeToEnd = endReq.time - anchor.time;
		double beatsToEndTime = TimeToBeats(timeToEnd, anchor.seg.bpm);

		// Start at the beat just before endReq.time
		for (int beatCount = int(ceil(beatsToEndTime)) - 1; beatCount > 0; --beatCount)
		{
			int candidateRow = anchor.seg.row + beatCount * RowsPerBeat;
			double timeToCandidate = BeatsToTime(beatCount, anchor.seg.bpm);
			double candidateTime = anchor.time + timeToCandidate;
			double timeFromCandidateToEnd = endReq.time - candidateTime;

			double aliasedAlignBpm = BeatsPerMinute(1.0, timeFromCandidateToEnd);
			double alignBpm = ClosestAliasBpm(aliasedAlignBpm, anchor.seg.bpm);
			double error = abs(anchor.seg.bpm / alignBpm - 1.0);

			if (error < toleranceMultiplier * BpmRegularityThreshold)
			{
				anchors.push_back(Anchor::Bpm(candidateRow, candidateTime, alignBpm, anchor.onset));

				adjustedEndReqRow = candidateRow + int(round(RowsPerBeat * (alignBpm / aliasedAlignBpm)));
				aligned = true;
				break;
			}
		}

		if (!aligned)
		{
			double aliasedAlignBpm = BeatsPerMinute(1.0, timeToEnd);
			double alignBpm = ClosestAliasBpm(aliasedAlignBpm, anchor.seg.bpm);
			double error = abs(anchor.seg.bpm / alignBpm - 1.0);
			if (error < toleranceMultiplier * BpmRegularityThreshold)
			{
				anchors.back().seg.bpm = alignBpm;
				adjustedEndReqRow = anchor.seg.row + int(round(RowsPerBeat * (alignBpm / aliasedAlignBpm)));
				aligned = true;
			}
			else
			{
				// Unsyncable--discard
				anchors.pop_back();
			}
		}
	}

	if (!aligned)
	{
		// Unable to align endAnchor with anything but the first anchor
		anchors.push_back(Anchor::Required(startReq.seg.row, startReq.time, startReq.seg.bpm, 0));
	}

	anchors.push_back(Anchor::Required(adjustedEndReqRow, endReq.time, endReq.seg.bpm, endReq.onset));
}

static Vector<Anchor> Fit(Span<TimeOnset> onsets, Span<OnsetBpmEstimate> bpmEstimates, Anchor& startReq, Anchor& endReq, int options = FitOptions::Default)
{
	bool alignToEnd = options & FitOptions::AlignToEnd;
	bool aliasToStartBpm = options & FitOptions::AliasToStartBpm;

	struct FitWindow
	{
		int startOnset;
		int endOnset;
		double targetTime;
		double testBpm;
		bool measure;
		bool synthetic;
	};

	struct FitCandidate
	{
		AlignedBeat beat = {};
		FitWindow window = {};
		double time = 0;
	};

	double halfBeatAtStartBpm = BeatsToTime(0.5, startReq.seg.bpm);

	Vector<FitWindow> windows;
	Vector<Anchor> anchors;
	anchors.push_back(Anchor::Required(startReq.seg.row, startReq.time, startReq.seg.bpm, 0));

	for (int i = 0; i < onsets.size() - 1; ++i)
	{
		windows.clear();

		double onsetTime = onsets[i].time;

		if (onsetTime <= anchors.back().time)
			continue;

		// Skip onsets within half a beat of the end boundary
		if (endReq.time - onsetTime < halfBeatAtStartBpm)
			continue;

		if (onsetTime <= startReq.time)
			continue;

		double estimatedBpm = bpmEstimates[i].bpm;
		double beatInterval = BeatsToTime(1.0, estimatedBpm);
		double startOnsetTime = anchors.back().time;
		int startOnset = anchors.back().onset;
		int lookAheadBeats = 4;
		int lookAheadMeasures = 1;
		int lastTestedOnset = i;

		// The thinking in this function is to lookahead for an onset we're confident lies on a beat. But sometimes
		// there is no such onset, but there are off-beat onsets that strongly signal the current BPM, and we can place
		// a beat where we have no onset. If they exist the BPM is almost certainly one of these:
		double syntheticBPMs[] = { anchors.back().seg.bpm, estimatedBpm };

		// Measures to test
		for (int measureAhead = 1; measureAhead <= lookAheadMeasures; ++measureAhead)
		{
			for (double testBpm : syntheticBPMs)
			{
				double testBeatInterval = BeatsToTime(1.0, testBpm);
				double measureInterval = testBeatInterval * 4.0;
				double targetTime = startOnsetTime + (measureAhead * measureInterval);
				double onsetWindow = testBeatInterval;

				// Test onsets near the measure boundary
				for (int j = i + 1; j < onsets.size(); ++j)
				{
					double onsetTime = onsets[j].time;

					if (onsetTime < targetTime - onsetWindow)
						continue;
					if (onsetTime > targetTime + onsetWindow)
						break;

					windows.push_back({ startOnset, j, onsetTime, testBpm, true, false });
				}

				// And just dump down a measure line and test everything in it
				int endOnset = i;
				while (endOnset + 1 < onsets.size() && onsets[endOnset + 1].time <= targetTime)
				{
					endOnset++;
				}

				windows.push_back({ startOnset, endOnset, targetTime, testBpm, true, true });
			}
		}

		// Beats to test
		for (int beatAhead = 1; beatAhead <= lookAheadBeats; ++beatAhead)
		{
			double targetTime = startOnsetTime + (beatAhead * beatInterval);
			double onsetWindow = beatInterval * 0.25;

			// Test onsets near the beat
			for (int j = i + 1; j < onsets.size(); ++j)
			{
				double onsetTime = onsets[j].time;

				if (onsetTime < targetTime - onsetWindow)
					continue;
				if (onsetTime > targetTime + onsetWindow)
					break;

				windows.push_back({ startOnset, j, onsetTime, estimatedBpm, false, false });
			}

			int endOnset = i;
			while (endOnset + 1 < onsets.size() && onsets[endOnset + 1].time <= targetTime)
			{
				endOnset++;
			}

			// And dump down the beat at each test bpm
			for (double testBpm : syntheticBPMs)
			{
				windows.push_back({ startOnset, endOnset, targetTime, testBpm, false, true });
			}
		}

		Anchor anchor = Anchor::Bpm(anchors.back().seg.row, anchors.back().time, estimatedBpm, i);
		FitCandidate best = {};
		for (FitWindow& w : windows)
		{
			Span<TimeOnset> window(onsets, w.startOnset, w.endOnset + 1);

			// If we reach the beat windows and we have a measure fit to an onset, don't
			// test beats
			if (!best.window.synthetic && best.window.measure && !w.measure)
				break;

			double endOnsetTime = window.back().time;
			lastTestedOnset = max(lastTestedOnset, w.endOnset);

			AlignedBeat beat = {};
			double beatTime = w.targetTime;

			if (w.synthetic)
			{
				double beatInterval = BeatsToTime(1.0, w.testBpm);
				double measureInterval = 4.0 * beatInterval;
				double timeSinceLastAnchor = endOnsetTime - anchor.time;
				double beatsElapsed = timeSinceLastAnchor / beatInterval;
				double measuresElapsed = timeSinceLastAnchor / measureInterval;
				double nextAnchorTime = w.measure ? anchor.time + ceil(measuresElapsed) * measureInterval
												  : anchor.time + ceil(beatsElapsed) * beatInterval;

				beatTime = SnapToNearestOnset(onsets, w.endOnset, nextAnchorTime, beatInterval);
				beat = MeasureBpmPlacement(window, anchor, nextAnchorTime, w.testBpm, beatTime);
			}
			else
			{
				beat = MeasureBpmPlacement(window, anchor, w.targetTime, w.testBpm);
			}

			if (beat.score > best.beat.score)
			{
				best.beat = beat;
				best.window = w;
				best.time = beatTime;
			}
		}

		if (best.beat.score > 0)
		{
			// We now have a time and a row we know works as a place to put a bpm change

			while (anchors.size() >= 2)
			{
				// It might be that we can get a better fit by dropping the last anchor we placed and aligning to this
				// new row from the anchor before that. This logic works recursively, but is only reliable if the BPMs
				// are consistent
				Anchor previousAnchor = anchors[anchors.size() - 2];

				double minBpmEstimate = INFINITY;
				double maxBpmEstimate = 0.0;
				for (int estimateIndex = previousAnchor.onset; estimateIndex <= anchor.onset; ++estimateIndex)
				{
					minBpmEstimate = min(minBpmEstimate, bpmEstimates[estimateIndex].bpm);
					maxBpmEstimate = max(maxBpmEstimate, bpmEstimates[estimateIndex].bpm);
				}
				if (abs(minBpmEstimate / maxBpmEstimate - 1.0) < 0.5*BpmRegularityThreshold)
				{
					Span<TimeOnset> currentWindow(onsets, best.window.startOnset, best.window.endOnset + 1);
					Span<TimeOnset> previousWindow(onsets, previousAnchor.onset, best.window.endOnset + 1);

					AlignedBeat currentCandidate = MeasureBpmPlacement(currentWindow, anchor, best.time, estimatedBpm);
					AlignedBeat previousCandidate = MeasureBpmPlacement(previousWindow, previousAnchor, best.time, bpmEstimates[previousAnchor.onset].bpm);

					if (previousCandidate.score > currentCandidate.score)
					{
						anchors.pop_back();
						best.beat = previousCandidate;

						continue;
					}
				}

				break;
			}

			int lastBpmRow = anchors.back().seg.row;
			int rowDelta = best.beat.row - lastBpmRow;
			double beatDelta = double(rowDelta) / RowsPerBeat;
			double timeDelta = best.time - anchors.back().time;
			double aliasedBpm = BeatsPerMinute(beatDelta, timeDelta);
			double anchorBpm = ClosestAliasBpm(aliasedBpm, estimatedBpm);
			int anchorRow = lastBpmRow + int(rowDelta * (anchorBpm / aliasedBpm));

			if ((anchorRow % RowsPerBeat) != 0)
			{
				// This choice of onset produced a BPM which when deliased moved us off beat. So either this BPM is
				// wrong or the BPM estimate is wrong. We always side with the estimate
				i = best.window.endOnset;
				continue;
			}

			if (rowDelta < RowsPerBeat || anchorBpm < MinSyncBPM || MaxSyncBPM < anchorBpm)
			{
				// If we have required bpms that don't lie on a beat we can be left with very short intervals to sync;
				// skip past these.
				i = lastTestedOnset;
				continue;
			}

			// Modify the last anchor's BPM to align timing to the position we found and at an anchor there for the next
			// iteration to overwrite
			double nextBpm = best.window.synthetic ? best.window.testBpm : anchorBpm;
			anchors.back().seg.bpm = anchorBpm;
			anchors.push_back(Anchor::Bpm(anchorRow, best.time, nextBpm, best.window.endOnset));
			
			i = best.window.endOnset - 1;
		}
		else
		{
			i = lastTestedOnset;
		}
	}

	// If the caller thinks the BPM is something weird oblige them. We dont do this while fitting
	// as the measure functions are pretty opinionated
	if (aliasToStartBpm)
	{
		anchors[0].seg.bpm = ClosestAliasBpm(anchors[0].seg.bpm, startReq.seg.bpm);
		for (int i = 1; i < anchors.size(); ++i)
		{
			auto& prev = anchors[i - 1];
			auto& anchor = anchors[i];
			double correctedBpm = ClosestAliasBpm(anchor.seg.bpm, prev.seg.bpm);

			double timeDelta = anchor.time - prev.time;
			double beats = TimeToBeats(timeDelta, prev.seg.bpm);
			anchor.seg.row = prev.seg.row + int(round(beats * RowsPerBeat));
			anchor.seg.bpm = correctedBpm;
		}
	}

	if (alignToEnd)
	{
		Align(anchors, startReq, endReq);
	}

	return anchors;
}

static void Refine(Vector<Anchor>& anchors, Span<TimeOnset> onsets, int quantization, double toleranceScale)
{
	VortexAssert(quantization > 0);
	Vector<Anchor> refined = anchors;

	// Snap onsets onto rows as potential new bpm changes
	int maxSnapRowDelta = max(1, quantization / 6);
	RowTime itr(anchors);
	RowTime itt(anchors);
	for (int i = 0; i < onsets.size(); ++i)
	{
		double onsetTime = onsets[i].time;
		int computedRow = itr.Row(onsetTime);
		int nearestSnap = ((computedRow + quantization / 2) / quantization) * quantization;
		double computedRowTime = itt.Time(nearestSnap);
		int rowDelta = abs(computedRow - nearestSnap);
		double timeDelta = abs(computedRowTime - onsetTime);
		if (timeDelta > 0.0 && rowDelta <= maxSnapRowDelta)
		{
			refined.push_back(Anchor::Onset(nearestSnap, onsetTime, anchors[itr.bpmIndex].seg.bpm, i, timeDelta));
		}
	}

	if (refined.size() <= 2)
	{
		// Can't do anything with this
		return;
	}

	// Inserting snapped onsets means we have some garbage we have to clean up. In particular, snapping can try to snap
	// an onset onto a row after an existing bpm. So, after sorting by row, we need to discard any anchors that aren't
	// going strictly forward in time.

	// Both of the following passes remove pairs of anchors that fail to produce sensible BPMs, but they don't check
	// that the next anchor along also works. Instead of doing that inline, just iterate
	int maxFixedPointIters = refined.size();

	std::sort(refined.begin(), refined.end(), [](const Anchor& a, const Anchor& b) {
		return (a.seg.row == b.seg.row) ? (a.time < b.time) : (a.seg.row < b.seg.row);
	});

	Vector<Anchor>& filtered = anchors;
	for (int fixedPointIter = 0; fixedPointIter < maxFixedPointIters; ++fixedPointIter)
	{
		filtered.clear();
		filtered.push_back(refined[0]);
		for (auto anchor : refined)
		{
			if (anchor.seg.row != filtered.back().seg.row && filtered.back().time < anchor.time)
			{
				// New row later in time, can keep
				filtered.push_back(anchor);

				// All other cases means we need to discard this anchor or the last anchor
			}
			else if (!anchor.Onset() && filtered.back().Onset())
			{
				// BPMs and required anchors win over onset anchors
				filtered.back() = anchor;
			}
			else if (anchor.Required())
			{
				// Required anchors win over other anchors
				filtered.back() = anchor;
			}
			else if (anchor.Onset() && filtered.back().Onset())
			{
				// Onset anchor nearest to the row wins
				if (anchor.delta < filtered.back().delta)
				{
					filtered.back() = anchor;
				}
			}
			else
			{
				// Discard
			}
		}

		if (refined.size() == filtered.size())
		{
			// Reached fixed point, stop
			break;
		}

		refined.swap(filtered);
	}

	// Check invariants after that nonsense
	for (int i = 1; i < refined.size(); ++i)
	{
		VortexAssert(refined[i-1].seg.row < refined[i].seg.row);
		VortexAssert((refined[i-1].time - refined[i].time) < 0x1p-16);
	}

	// Filter refinement anchors that would perturb BPM too much or too little
	for (int fixedPointIters = 0; fixedPointIters < maxFixedPointIters; ++fixedPointIters)
	{
		filtered.clear();
		filtered.push_back(refined[0]);

		for (int i = 1; i < refined.size() - 1; ++i)
		{
			Anchor anchor = refined[i];
			Anchor prev = filtered.back();
			Anchor next = refined[i + 1];

			bool nearRequired = prev.Required() || next.Required();

			// Anchor BPMs at this point do not align to onset anchors, so we have to compute the BPM the previous
			// anchor will have if we keep this onset anchor
			double timeDelta = anchor.time - prev.time;
			int rowDelta = anchor.seg.row - prev.seg.row;
			double beatDelta = double(rowDelta) / RowsPerBeat;
			double deltaBpm = BeatsPerMinute(beatDelta, timeDelta);

			double prevToNextTimeDelta = next.time - prev.time;
			int prevToNextRowDelta = next.seg.row - prev.seg.row;
			double prevToNextBeatDelta = double(prevToNextRowDelta) / RowsPerBeat;
			double prevToNextBpm = BeatsPerMinute(prevToNextBeatDelta, prevToNextTimeDelta);
			double anchorRowTime = prev.time + BeatsToTime(beatDelta, prevToNextBpm);

			bool goodBpm = abs(anchor.seg.bpm / deltaBpm - 1.0) <= toleranceScale * BpmRegularityThreshold;
			bool pointless = anchor.Onset() && abs(anchorRowTime - anchor.time) < (AnchorTimeThreshold / toleranceScale);

			if (anchor.Required())
			{
				filtered.push_back(anchor);
			}
			else if (!pointless)
			{
				if (anchor.Onset() || nearRequired)
				{
					if (goodBpm)
					{
						filtered.push_back(anchor);
					}
				}
				else
				{
					filtered.push_back(anchor);
				}
			}
		}

		filtered.push_back(refined.back());

		if (refined.size() == filtered.size())
		{
			// Reached fixed point, stop
			break;
		}

		refined.swap(filtered);
	}

	// Recompute bpms with remaining anchors
	for (int i = 0; i < refined.size() - 1; ++i)
	{
		Anchor anchor = refined[i];
		Anchor next = refined[i + 1];

		int rowDelta = next.seg.row - anchor.seg.row;
		double beatDelta = double(rowDelta) / RowsPerBeat;
		double timeDelta = next.time - anchor.time;

		VortexAssert(rowDelta > 0);
		VortexAssert(timeDelta > 0.0);

		refined[i] = Anchor::Refined(anchor, BeatsPerMinute(beatDelta, timeDelta));
	}

	anchors.swap(refined);
}

AutoSyncResult AutoSync(const Vector<Onset>& sampleOnsets, int musicFrameCount, int musicSampleRate, const Vector<BpmChange>& inputBpmChanges, double offset, PreserveOptions preserveOptions, SyncMode mode)
{
	PreserveOptions options = preserveOptions;

	// AV will always have at least one BPM change but for testing it's an annoying API
	Span<BpmChange> existingBpmChanges = inputBpmChanges;
	Vector<BpmChange> noBpmChanges;
	if (inputBpmChanges.empty())
	{
		noBpmChanges.push_back(BpmChange(0, 120.0));
		existingBpmChanges = noBpmChanges;
	}

	double audioDuration = double(musicFrameCount) / double(musicSampleRate);
	double resultOffset = offset;
	double inputOffset = offset;
	double normalizationScale = 1.0;
	int regionOnsetStart = 0;
	int regionOnsetEnd = 0;

	// MeasureBeatAlignment can detect single bpms pretty reliably, so we have a couple early out tests.
	// Whether we can set the offset or not changes what we have to do to make this work
	bool singleBpmMode = (mode == SyncMode::SingleBpm);
	bool regionMode = (options.flags & PreserveOptions::Region) != 0;
	bool preservingSingleBpm = (options.flags & PreserveOptions::Preserve) && (existingBpmChanges.size() == 1);
	bool syncingRowZero = (options.flags == PreserveOptions::None) || preservingSingleBpm;
	bool syncingEveryBeat = (mode >= SyncMode::Beats);
	bool canEarlyOut = (singleBpmMode || syncingRowZero) && !syncingEveryBeat;
	double earlyOutThreshold = singleBpmMode ? 0.0 : 0.33;

	// If the file is newly loaded we can be a bit more aggressive and also set the offset
	bool freshFile = existingBpmChanges.size() == 1
				  && existingBpmChanges[0].bpm == 120.0
				  && existingBpmChanges[0].row == 0;
	bool cleanSlate = (options.flags == PreserveOptions::None) || freshFile;
	bool preserveBpms = (options.flags & PreserveOptions::Preserve) && !cleanSlate;
	bool syncEntireFile = !preserveBpms && !regionMode;

	double referenceBpm = regionMode ? preserveOptions.regionStartBpm : existingBpmChanges[0].bpm;

	Vector<Anchor> anchors;
	Vector<TimeOnset> timeOnsetsBuffer(sampleOnsets.size(), {});
	Span<TimeOnset> timeOnsets(timeOnsetsBuffer);

	//
	// First thing to do is turn onset times in samples to seconds and get local BPM estimates for all of them. These
	// estimates being good is why everything else in this file works at all
	//
	// EstimateLocalBpm works best on very short windows, around a beat long, but it's possible to rule out implausible
	// local variations by looking further ahead. So on the first pass we get short time scale estimates and on the
	// second we use a sliding histogram as lookahead window to incorporate more distant information.
	//

	for (int i = 0; i < sampleOnsets.size(); ++i)
	{
		timeOnsetsBuffer[i].time = double(sampleOnsets[i].pos) / double(musicSampleRate);
		timeOnsetsBuffer[i].strength = sampleOnsets[i].strength;
	}

	// Discard any onsets before the offset
	{
		RowTime it(existingBpmChanges, inputOffset);
		double startTime = it.Time(0);
		int startIndex = 0;
		for (; startIndex < timeOnsets.size(); ++startIndex)
		{
			if (timeOnsets[startIndex].time >= startTime)
				break;
		}

		timeOnsets = Span<TimeOnset>(timeOnsets, startIndex, timeOnsets.size());
	}

	// Discard all onsets outside the sync region
	if (regionMode)
	{
		RowTime it(existingBpmChanges, inputOffset);
		double regionStartTime = it.Time(options.regionStartRow);
		double regionEndTime = it.Time(options.regionEndRow);
		int regionStartIndex = 0;
		int regionEndIndex = 0;
		for (; regionStartIndex < timeOnsets.size(); ++regionStartIndex)
		{
			if (timeOnsets[regionStartIndex].time >= regionStartTime)
				break;
		}

		for (regionEndIndex = regionStartIndex + 1; regionEndIndex < timeOnsets.size(); ++regionEndIndex)
		{
			if (timeOnsets[regionEndIndex].time >= regionEndTime)
				break;
		}

		timeOnsets = Span<TimeOnset>(timeOnsets, regionStartIndex, regionEndIndex);
	}

	if (timeOnsets.empty())
		return { inputBpmChanges, inputOffset };

	Vector<OnsetBpmEstimate> onsetBpmEstimates(timeOnsets.size(), {0.0, 0.0});

	// BPMs are slightly anti-causal locally so get the initial estimate by scanning backwards
	int last = onsetBpmEstimates.size() - 1;
	onsetBpmEstimates[last] = EstimateLocalBpm(timeOnsets, last, referenceBpm, nullptr);
	for (int i = last - 1; i >= 0; --i)
	{
		onsetBpmEstimates[i] = EstimateLocalBpm(timeOnsets, i, onsetBpmEstimates[i + 1].bpm, nullptr);
	}

	constexpr int LookaheadBeats = 16;
	auto slidingHistogram = SlidingHistogram::Build(onsetBpmEstimates, timeOnsets, LookaheadBeats);
	for (int i = 0; i < onsetBpmEstimates.size(); ++i)
	{
		slidingHistogram.Advance(i);
		onsetBpmEstimates[i] = EstimateLocalBpm(timeOnsets, i, onsetBpmEstimates[i].bpm, &slidingHistogram);
	}

	// Occasionally the estimates we get are out by 1.5x; if there's enough beat stress information in the onset
	// strengths we can detect this
	{
		double pulseMultiplier = FixTripletBPMs(timeOnsets, Histogram::Build(onsetBpmEstimates).Peak());
		if (pulseMultiplier != 1.0)
		{
			for (OnsetBpmEstimate& estimate : onsetBpmEstimates)
			{
				estimate.bpm *= pulseMultiplier;
			}
		}
	}

	// If we're preserving BPMs then we want all our estimates to reflect these BPMs
	if (preserveBpms)
	{
		RowTime it(existingBpmChanges, inputOffset);
		for (int i = 0; i < onsetBpmEstimates.size(); ++i)
		{
			it.Row(timeOnsets[i].time);
			onsetBpmEstimates[i].bpm = ClosestAliasBpm(onsetBpmEstimates[i].bpm, it.bpms[it.bpmIndex].bpm);
		}
	}

	// The histogram is used to find the peak BPM and as a weight when filtering the BPM estimates
	Histogram bpmHistogram = Histogram::Build(onsetBpmEstimates);

	// The most common BPM in the file. We always want to work with bpms in the [60,120) range, so if the peak BPM is
	// higher we scale all estimates into range then reverse that on output. The peak BPM from the histogram can be off
	// from the true BPM of the file so we recompute it from raw estimates then use MeasureBeatAlignment on nearby
	// BPMs to get a better estimate. This step is where we snap onto round BPMs if the song is locked to one
	double peakBpm = bpmHistogram.Peak();
	double histogramPeak = peakBpm;
	double bestPeakConfidence = 0.0;
	int bestPeakOnset = 0;
	int peakBpmCount = 1;
	for (OnsetBpmEstimate& estimate : onsetBpmEstimates)
	{
		if (abs(estimate.bpm - histogramPeak) <= 1.0)
		{
			peakBpm += estimate.bpm;
			peakBpmCount++;
		}
	}
	peakBpm /= peakBpmCount;

	if (peakBpm < 95.0)
	{
		// Nobody wants tiny bpms. This is an addition to the normalization below but
		// is not undone on output
		peakBpm *= 2.0;
		for (OnsetBpmEstimate& estimate : onsetBpmEstimates)
		{
			estimate.bpm *= 2.0;
		}
	}

	{
		int nOnsetsToTest = std::min(20, timeOnsets.size());

		// Test the peak bpm and a few rounded bpms around it
		Vector<double> candidateBpms;
		candidateBpms.push_back(peakBpm);
		double roundedPeakBpm = round(peakBpm);
		for (double bpmOffset = -2.0; bpmOffset <= 2.0; bpmOffset += 1.0)
		{
			candidateBpms.push_back(roundedPeakBpm + bpmOffset);
		}

		for (double candidateBpm : candidateBpms)
		{
			double interval = BeatsToTime(1.0, candidateBpm);

			// Test each of the first N onsets as potential offsets
			for (int onsetIndex = 0; onsetIndex < nOnsetsToTest; ++onsetIndex)
			{
				double confidence = timeOnsets[onsetIndex].strength * MeasureBeatAlignment(Span<TimeOnset>(timeOnsets, onsetIndex, timeOnsets.size()), interval);

				// Nudge too-close-to-call differences in confidence towards round bpms
				if (round(candidateBpm) != candidateBpm)
				{
					confidence *= .995;
				}

				if (confidence > bestPeakConfidence)
				{
					peakBpm = candidateBpm;
					bestPeakConfidence = confidence;
					bestPeakOnset = onsetIndex;
				}
			}
		}

		// Adjust confidence so the early out threshold does not use the onset strength. Weighting by strength made
		// sense to score early onsets against one another but if all early onsets are weak compared to onsets later in
		// the song the confidence may be erroneously weighted low.
		bestPeakConfidence /= timeOnsets[bestPeakOnset].strength;
	}

	while (peakBpm / normalizationScale > AnchorBpm * 2.0) normalizationScale *= 2.0;
	while (peakBpm / normalizationScale < AnchorBpm)	   normalizationScale /= 2.0;
	if (normalizationScale != 1.0)
	{
		peakBpm /= normalizationScale;
		for (OnsetBpmEstimate& estimate : onsetBpmEstimates)
		{
			estimate.bpm /= normalizationScale;
		}
	}

	// Normalize BPMs and rebuild histogram
	for (OnsetBpmEstimate& estimate : onsetBpmEstimates)
	{
		estimate.bpm = ClosestAliasBpm(estimate.bpm, peakBpm);
	}

	bpmHistogram = Histogram::Build(onsetBpmEstimates).Normalized();

	//
	// Weight onset confidences by histogram support
	//
	// Filter BPM estimates. This is pretty aggressive.
	// 1) BPMs are rates, so harmonic mean
	// 2) Raised cosine fall-off, same as hamming window
	// 3) Weight by confidence and histogram support. We do this by reweighting estimate confidence, and it's where most
	//    of the weighting effect comes from. This reweighting makes this totally unlike a linear filter, this is not a
	//    moving average
	//
	// Reason for 3: For BPMs we have a pretty strong expectation that they're well approximated by a piece-wise
	// constant function of time, and if that's the case, we have strong histogram peaks on those constants. Otoh if
	// they're drifty the histogram will be more spread out and the BPMs will be weighted more evenly.
	//

	for (OnsetBpmEstimate& estimate : onsetBpmEstimates)
		estimate.confidence *= bpmHistogram.Sample(estimate.bpm);
	for (int iters = 0; iters < 15; ++iters)
	{
		double windowRadiusInSeconds = 4.0 * 60. / peakBpm;

		Vector<OnsetBpmEstimate> originalBpms = onsetBpmEstimates;

		int windowStartIndex = 0;
		int windowEndIndex = 0;

		for (int i = 0; i < timeOnsets.size(); ++i)
		{
			double onsetTime = timeOnsets[i].time;
			double windowStart = onsetTime - windowRadiusInSeconds;
			double windowEnd = onsetTime + windowRadiusInSeconds;

			while (windowStartIndex < timeOnsets.size() && timeOnsets[windowStartIndex].time < windowStart)
				windowStartIndex++;
			while (windowEndIndex < timeOnsets.size() && timeOnsets[windowEndIndex].time <= windowEnd)
				windowEndIndex++;

			int windowSize = windowEndIndex - windowStartIndex;

			if (windowSize > 2)
			{
				double sumWeightedReciprocals = 0.0;
				double sumWeights = 0.0;
				for (int j = windowStartIndex; j < windowEndIndex; ++j)
				{
					double time = timeOnsets[j].time;
					double t = fabs(time - onsetTime) / windowRadiusInSeconds;
					double weight = (0.54 + 0.46 * cos(Pi * t)) * originalBpms[j].confidence;
					sumWeightedReciprocals += weight / originalBpms[j].bpm;
					sumWeights += weight;
				}
				if (sumWeightedReciprocals > 0.0)
				{
					double harmonicMean = sumWeights / sumWeightedReciprocals;
					onsetBpmEstimates[i].bpm = harmonicMean;
				}
			}
			else
			{
				onsetBpmEstimates[i].bpm = peakBpm;
			}
		}
	}

	// Set the offset by finding a good onset to align to
	if (syncEntireFile)
	{
		int syncOnset = 0;
		double bestScore = 0.0;

		double fourMeasures = BeatsToTime(16.0, peakBpm);
		Histogram earlyBpmsHistogram = {};
		for (int i = 0; i < timeOnsets.size(); ++i)
		{
			if ((timeOnsets[i].time - timeOnsets[0].time) >= fourMeasures)
			{
				earlyBpmsHistogram = Histogram::Build(Span<OnsetBpmEstimate>(onsetBpmEstimates, 0, i));
				break;
			}
		}

		double syncBpm = earlyBpmsHistogram.Peak() * normalizationScale;
		for (int i = 0; i < timeOnsets.size(); ++i)
		{
			if ((timeOnsets[i].time - timeOnsets[0].time) >= fourMeasures)
				break;

			double interval = BeatsToTime(1.0, syncBpm);
			double oneMeasure = interval * 4.0;
			int endIndex = i;
			for(; endIndex < timeOnsets.size(); ++endIndex)
			{
				if ((timeOnsets[endIndex].time - timeOnsets[i].time) >= oneMeasure)
				{
					break;
				}
			}

			double score = MeasureBeatAlignment(Span<TimeOnset>(timeOnsets, i, endIndex), interval);

			if (score > bestScore)
			{
				bestScore = score;
				syncOnset = i;
			}
		}

		// Walk backwards to find the earliest onset that maintains BPM regularity
		while (true)
		{
			bool stepped = false;
			double syncTime = timeOnsets[syncOnset].time;
			for (int i = syncOnset - 1; i >= 0; --i)
			{
				double timeDelta = syncTime - timeOnsets[i].time;
				double beatDelta = max(1.0, round(TimeToBeats(timeDelta, syncBpm)));
				double inducedBpm = ClosestAliasBpm(BeatsPerMinute(beatDelta, timeDelta), syncBpm);
				double mpbError = abs(syncBpm / inducedBpm - 1.0);
				if (mpbError < 4.0 * BpmRegularityThreshold)
				{
					syncOnset = i;
					stepped = true;
					break;
				}
			}

			if (!stepped)
				break;
		}

		// Only use the onset we got if it's actually near the start of the file. Exact limit is pretty arbitrary
		constexpr double OffsetLimit = 4.0;
		if (timeOnsets[syncOnset].time < OffsetLimit)
		{
			resultOffset = -timeOnsets[syncOnset].time;
		}
		else if (timeOnsets[0].time < OffsetLimit)
		{
			resultOffset = -timeOnsets[0].time;
		}
	}


	// At this point we know how good the peak BPM is and have set the offset, so we might be able to early out here
	if (canEarlyOut && bestPeakConfidence > earlyOutThreshold)
	{
		if (!regionMode)
		{
			// Refine the offest further by ensuring we align to whatever onset we derived the single BPM from
			double peakOnsetTime = timeOnsets[bestPeakOnset].time;
			double interval = BeatsToTime(1.0, peakBpm);
			double offsetTime = -resultOffset;
			double beats = round((peakOnsetTime - offsetTime) / interval);
			if (beats >= 0)
				resultOffset = -(peakOnsetTime - beats * interval);

			anchors.push_back(Anchor::Required(0, -resultOffset, peakBpm, 0));

			goto output;
		}
		else
		{
			RowTime it(existingBpmChanges, inputOffset);
			double regionStartTime = it.Time(options.regionStartRow);
			double regionEndTime = it.Time(options.regionEndRow);

			double bpm = peakBpm * normalizationScale;
			normalizationScale = 1.0;

			Anchor regionStart = Anchor::Required(options.regionStartRow, regionStartTime, bpm, 0);
			Anchor startReq = Anchor::Required(options.regionStartRow, regionStartTime, options.regionStartBpm, 0);
			Anchor endReq = Anchor::Required(options.regionEndRow, regionEndTime, options.regionEndBpm, 0);

			anchors.push_back(regionStart);

			if (regionEndTime < audioDuration)
			{
				Align(anchors, startReq, endReq);
			}

			goto output;
		}
	}

	//
	// Fit: assign onsets to beats
	//

	if (syncEntireFile)
	{
		// Set up the fit as one big range
		double startTime = -resultOffset;
		Anchor startReq = Anchor::Required(0, startTime, peakBpm, 0);

		double audioEndTime = audioDuration;
		double measureDuration = BeatsToTime(4.0, startReq.seg.bpm);
		double sentinelTime = audioEndTime + measureDuration;
		int sentinelRow = int(TimeToBeats(sentinelTime - startTime, peakBpm) * RowsPerBeat);
		Anchor sentinel = Anchor::Required(sentinelRow, sentinelTime, startReq.seg.bpm, 0);

		Span<TimeOnset> onsetsSpan(timeOnsets, 0, timeOnsets.size());
		Span<OnsetBpmEstimate> bpms(onsetBpmEstimates, 0, onsetBpmEstimates.size());
		anchors = Fit(onsetsSpan, bpms, startReq, sentinel, FitOptions::Default);
	}
	else
	{
		// Big song and dance to respect existing BPMs
		Vector<Anchor> requiredAnchors;

		if (regionMode && !preserveBpms)
		{
			// Region mode without preserve: required bpms are the region bounds only
		}
		else if (regionMode)
		{
			// Region mode with preserve: only add BPMs within region bounds
			int regionStartRow = int(options.regionStartRow / normalizationScale);
			int regionEndRow = int(options.regionEndRow / normalizationScale);
			RowTime it(existingBpmChanges, inputOffset);
			for (BpmChange anchor : existingBpmChanges)
			{
				int row = int(anchor.row / normalizationScale);
				if (row > regionStartRow && row < regionEndRow)
				{
					double bpm = anchor.bpm / normalizationScale;
					double time = it.Time(anchor.row);
					requiredAnchors.push_back(Anchor::Required(row, time, bpm, 0));
				}
			}
		}
		else
		{
			// Preserve mode without region: add all BPMs
			RowTime it(existingBpmChanges, inputOffset);
			for (BpmChange anchor : existingBpmChanges)
			{
				int row = int(anchor.row / normalizationScale);
				double bpm = anchor.bpm / normalizationScale;
				double time = it.Time(anchor.row);
				requiredAnchors.push_back(Anchor::Required(row, time, bpm, 0));
			}
		}

		if (regionMode)
		{
			// Song and dance continues for regions
			RowTime it(existingBpmChanges, inputOffset);

			int startRow = int(options.regionStartRow / normalizationScale);
			double startBpm = options.regionStartBpm / normalizationScale;
			double startTime = it.Time(options.regionStartRow);
			Anchor startReq = Anchor::Required(startRow, startTime, startBpm, 0);
			requiredAnchors.push_back(startReq);

			int endRow = int(options.regionEndRow / normalizationScale);
			double endBpm = options.regionEndBpm / normalizationScale;
			double endTime = it.Time(options.regionEndRow);
			Anchor endReq = Anchor::Required(endRow, endTime, endBpm, 0);
			requiredAnchors.push_back(endReq);

			std::sort(requiredAnchors.begin(), requiredAnchors.end(), [](const Anchor& a, const Anchor& b) {
				return a.seg.row < b.seg.row;
			});

			// Usually Fit() will add the first row as a required anchor, but in a region, we don't fit the first row,
			// so insert whatever BPM at row zero will put the region at the right row. We'll discard it on output
			if (startRow != 0)
			{
				double beatDelta = double(startReq.seg.row) / RowsPerBeat;
				double timeDelta = startReq.time + inputOffset;
				double initialBpm = BeatsPerMinute(beatDelta, timeDelta);
				anchors.push_back(Anchor::Required(0, -inputOffset, initialBpm, 0));
			}

			// Filter timeOnsets to region bounds for Refine
			regionOnsetEnd = timeOnsets.size();
			for (int i = 0; i < timeOnsets.size(); ++i)
			{
				if (timeOnsets[i].time >= startTime)
				{
					regionOnsetStart = i;
					break;
				}
			}
			for (int i = regionOnsetStart; i < timeOnsets.size(); ++i)
			{
				if (timeOnsets[i].time > endTime)
				{
					regionOnsetEnd = i;
					break;
				}
			}
		}

		// To simplify the fit loop we add a sentinel BPM
		{
			double audioEndTime = audioDuration;
			double endBpm = requiredAnchors.back().seg.bpm;
			double measureDuration = BeatsToTime(4.0, endBpm);
			double sentinelTime = audioEndTime + measureDuration;

			int lastRow = requiredAnchors.back().seg.row;
			double lastTime = requiredAnchors.back().time;
			int sentinelRow = lastRow + int(TimeToBeats(sentinelTime - lastTime, endBpm) * RowsPerBeat);

			requiredAnchors.push_back(Anchor::Required(sentinelRow, sentinelTime, endBpm, 0));
		}

		// If we have a region we can skip the last bpm range
		int sentinelIndex = requiredAnchors.size() - 1;
		int reqEnd = sentinelIndex;
		if (regionMode)
		{
			reqEnd -= 1;
		}

		for (int reqIndex = 0; reqIndex < reqEnd; ++reqIndex)
		{
			Anchor startReq = requiredAnchors[reqIndex];
			Anchor endReq = requiredAnchors[reqIndex + 1];

			// Don't attempt to sync what is already synced
			if (endReq.seg.row - startReq.seg.row <= 96)
			{
				anchors.push_back(startReq);
				if (regionMode && reqIndex == reqEnd - 1)
					anchors.push_back(endReq);
				continue;
			}

			int fitOptions = FitOptions::Default;
			if (preserveBpms)
				fitOptions |= FitOptions::AliasToStartBpm;
			if ((reqIndex + 1) != sentinelIndex)
				fitOptions |= FitOptions::AlignToEnd;

			int onsetStart = 0;
			int onsetEnd = timeOnsets.size();
			for (int i = 0; i < timeOnsets.size(); ++i)
			{
				double onsetTime = timeOnsets[i].time;
				if (onsetTime >= startReq.time)
				{
					onsetStart = i;
					break;
				}
			}

			for (int i = onsetStart + 1; i < timeOnsets.size(); ++i)
			{
				double onsetTime = timeOnsets[i].time;
				if (onsetTime > endReq.time)
				{
					onsetEnd = i;
					break;
				}
			}

			Span<TimeOnset> onsetsSpan(timeOnsets, onsetStart, onsetEnd);
			Span<OnsetBpmEstimate> bpms(onsetBpmEstimates, onsetStart, onsetEnd);

			Vector<Anchor> range = Fit(onsetsSpan, bpms, startReq, endReq, fitOptions);

			for (auto anchor : range)
			{
				anchor.onset += onsetStart;
				anchors.push_back(anchor);
			}

			if (fitOptions & FitOptions::AlignToEnd)
			{
				int rowDelta = anchors.back().seg.row - endReq.seg.row;
				if (rowDelta)
				{
					for (int laterReqIndex = reqIndex + 1; laterReqIndex <= sentinelIndex; ++laterReqIndex)
					{
						requiredAnchors[laterReqIndex].seg.row += rowDelta;
					}
				}

				if (!regionMode)
				{
					VortexAssert(range.size() >= 2);
					anchors.pop_back();
				}
			}
		}
	}

	//
	// Refine: snap any onsets we missed onto beats. This will also clean up anchors that barely change the BPM
	//

	{
		Span<TimeOnset> refineOnsets = regionMode ? Span<TimeOnset>(timeOnsets, regionOnsetStart, regionOnsetEnd) : timeOnsets;

		switch (mode)
		{
			case SyncMode::SingleBpm:
			case SyncMode::Clean:
			{
				Refine(anchors, refineOnsets, RowsPerBeat, 1.0);	  // Snap onsets onto beats
			} break;
			case SyncMode::Beats:
			{
				Refine(anchors, refineOnsets, RowsPerBeat, 2.0);	  // Snap onsets onto beats
				Refine(anchors, refineOnsets, RowsPerBeat / 2, 2.0);  // Snap onsets onto 8ths
				Refine(anchors, refineOnsets, RowsPerBeat / 4, 2.0);  // Snap onsets onto 16ths
				Refine(anchors, refineOnsets, RowsPerBeat / 6, 2.0);  // Snap onsets onto 24ths
			} break;
			case SyncMode::SnapEverything:
			{
				Refine(anchors, refineOnsets, RowsPerBeat, 4.0);	  // Snap onsets onto beats
				Refine(anchors, refineOnsets, RowsPerBeat / 2, 4.0);  // Snap onsets onto 8ths
				Refine(anchors, refineOnsets, RowsPerBeat / 4, 4.0);  // Snap onsets onto 16ths
				Refine(anchors, refineOnsets, RowsPerBeat / 6, 4.0);  // Snap onsets onto 24ths
				Refine(anchors, refineOnsets, RowsPerBeat / 8, 4.0);  // Keep going
				Refine(anchors, refineOnsets, RowsPerBeat / 12, 4.0); // Keep going
				Refine(anchors, refineOnsets, RowsPerBeat / 16, 4.0); // Keep going
			} break;
		}
	}

output:
	Vector<BpmChange> result(anchors.size());
	int regionEndRowDelta = 0;

	if (regionMode)
	{
		// Region: Splice anchors into input BPMs
		int regionStartRow = options.regionStartRow;
		int regionEndRow = options.regionEndRow;

		int syncedEndRow = int(anchors.back().seg.row * normalizationScale);
		regionEndRowDelta = syncedEndRow - regionEndRow;

		for (BpmChange bpm : existingBpmChanges)
		{
			if (bpm.row < regionStartRow)
			{
				result.push_back(bpm);
			}
		}

		double previousBpm = result.back().bpm;
		for (Anchor anchor : anchors)
		{
			BpmChange bpm = anchor.seg;
			bpm.bpm *= normalizationScale;
			bpm.row = int(bpm.row * normalizationScale);
			if (bpm.row >= regionStartRow && bpm.row <= syncedEndRow)
			{
				if (anchor.Required() || bpm.bpm != previousBpm)
				{
					result.push_back(bpm);
					previousBpm = bpm.bpm;
				}
			}
		}

		for (BpmChange bpm : existingBpmChanges)
		{
			if (bpm.row > regionEndRow)
			{
				bpm.row += regionEndRowDelta;
				result.push_back(bpm);
			}
		}
	}
	else
	{
		// No region: write a new set of BPMs
		double previousBpm = 0.0;
		for (Anchor anchor : anchors)
		{
			BpmChange bpm = anchor.seg;
			bpm.bpm *= normalizationScale;
			bpm.row = int(bpm.row * normalizationScale);
			if (anchor.Required() || bpm.bpm != previousBpm)
			{
				result.push_back(bpm);
				previousBpm = bpm.bpm;
			}
		}
	}

#ifndef VORTEX_DISABLE_ASSERTS
	// Invariant checks: input BPM and region bound times cannot change
	if (!syncEntireFile && result.size() > 1)
	{
		RowTime inputIt(existingBpmChanges, inputOffset);
		RowTime outputRowIt(result, resultOffset);
		RowTime outputTimeIt(result, resultOffset);
		for (auto bpm : existingBpmChanges)
		{
			if (regionMode && !preserveBpms && bpm.row >= options.regionStartRow && bpm.row <= options.regionEndRow)
				continue;
			double inputTime = inputIt.Time(bpm.row);
			int nearestRow = outputRowIt.Row(inputTime);
			double outputTime = outputTimeIt.Time(nearestRow);
			VortexAssert(abs(outputTime - inputTime) < 0x1p-16);
		}
	}
	if (regionMode)
	{
		RowTime inputIt(existingBpmChanges, inputOffset);
		double startTime = inputIt.Time(options.regionStartRow);
		double endTime = inputIt.Time(options.regionEndRow);

		RowTime outputRowIt(result, resultOffset);
		RowTime outputTimeIt(result, resultOffset);
		int nearestStartRow = outputRowIt.Row(startTime);
		double outputStartTime = outputTimeIt.Time(nearestStartRow);
		VortexAssert(abs(outputStartTime - startTime) < 0x1p-16);

		if (endTime < audioDuration)
		{
			int nearestEndRow = outputRowIt.Row(endTime);
			double outputEndTime = outputTimeIt.Time(nearestEndRow);
			VortexAssert(abs(outputEndTime - endTime) < 0x1p-16);
		}
	}
#endif

	return { result, resultOffset, regionEndRowDelta };
}

// ================================================================================================
// Onset refinement.

void RefineOnsets(Vector<Onset>& onsets, const float* samples, int samplerate, int numFrames)
{
	// If you look at this and have some idea of the time scales involved you will probably
	// conclude it can't be doing anything worthwhile but I promise you this load bearing
	// on the quality of the BPMs we find

	// Default high-pass filter strength
	constexpr double HighPassCutoffHz = 4632.0;
	// Yeah I don't know why this value works. Note 2rd-order butterworth has gentle slopes
	constexpr double LowPassCutoffHz = 5733.0;

	constexpr double WindowMs = 5.0;
	constexpr int ThresholdRatio = 4;
	int windowSamples = int(samplerate * WindowMs / 1000.0);

	Vector<float> filteredSamples(numFrames, 0.0f);
	{
		Iir::Butterworth::HighPass<3> hp;
		Iir::Butterworth::LowPass<2> lp;
		hp.setup(samplerate, HighPassCutoffHz);
		lp.setup(samplerate, LowPassCutoffHz);
		for (int i = 0; i < numFrames; ++i)
		{
			filteredSamples[i] = lp.filter(hp.filter(samples[i]));
		}
	}

	// After high passing we definitely have a zero centered signal, so high energy impacts
	// look like rapid oscillations around zero, and we only care about how fast it's oscillating,
	// not its particular direction. So to refine onsets we want to look for events in this signal:
	Vector<float> absDiff(numFrames, 0.0f);
	for (int i = 1; i < numFrames; ++i)
	{
		absDiff[i] = fabsf(filteredSamples[i] - filteredSamples[i - 1]);
	}

	int minWindow = 0;
	for (Onset& onset : onsets)
	{
		int windowStart = max(minWindow, onset.pos - windowSamples);

		// We want to move this onset onto the beginning of the transient that triggered it.
		// The abs-difference signal is noisy so we can't just look for a local minimum. Instead
		// look for two values sufficiently different in magnitude (determined by ThresholdRatio),
		// and say the smaller of the two is probably a better choice of onset position.
		//
		// But that new onset position is just another onset position like we started with, so if
		// the above logic works, we should be able to apply it repeatedly. So do that, but shrink
		// the window each time to prevent us shooting off to irrelevant places in the waveform
		int onsetPos = onset.pos;

		for (int nIter = 0; nIter < 5; ++nIter)
		{
			windowStart = max(minWindow, onsetPos - (windowSamples >> nIter));

			float maxDiffValue = 0.0f;
			int maxDiffSample = onsetPos;

			for (int s = windowStart; s < onsetPos; ++s)
			{
				if (absDiff[s] > maxDiffValue)
				{
					maxDiffValue = absDiff[s];
					maxDiffSample = s;
				}
			}

			float threshold = maxDiffValue / ThresholdRatio;
			for (int s = maxDiffSample - 1; s >= windowStart; --s)
			{
				if (absDiff[s] <= threshold)
				{
					onsetPos = s;
					break;
				}
			}
		}

		onset.pos = onsetPos;
		minWindow = onset.pos + 1;
	}
}

}; // namespace Vortex