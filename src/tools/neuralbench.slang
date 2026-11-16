// neuralbench's kernels (tools/neuralbench.cpp): a radiance cache as the neural methods keep one, in plain compute (no
// tensor cores, no cooperative vectors or matrices): a multiresolution hash grid of learned features over the unit cube
// (Mueller et al. 2022's encoding: NB_LEVEL_COUNT levels of NB_TABLE_SIZE entries of 2 features, dense where a level
// fits, hashed where it doesn't) decoded by a small network (NB_HIDDEN_COUNT layers of NB_WIDTH neurons with ReLUs, no
// biases, to RGB; 0 layers: the features' weighted sum), queried (ComputeInfer) and trained (a step: ComputeTrain,
// ComputeWeightGradient, ComputeWeightStep, ComputeGridStep) on a made up scene (NbTarget); and, to compare, a cache
// without learning (cells of running means, see NbMeanEntry). what they cost and how fast they learn, not how they look
// in a renderer. bound as the client's layouts are: the push constants in set 0, set 1's resources a parameter block

#ifndef NB_LEVEL_COUNT
#define NB_LEVEL_COUNT 8
#endif
#ifndef NB_TABLE_SIZE_LOG2
#define NB_TABLE_SIZE_LOG2 17
#endif
#ifndef NB_WIDTH
#define NB_WIDTH 32
#endif
#ifndef NB_HIDDEN_COUNT
#define NB_HIDDEN_COUNT 2
#endif
// a level's resolution over the last's, in 16ths (the coarsest is NB_BASE_RESOLUTION cells a side)
#ifndef NB_GROWTH_16THS
#define NB_GROWTH_16THS 32
#endif
#define NB_BASE_RESOLUTION 16
#define NB_FEATURE_COUNT 2
#define NB_TABLE_SIZE (1u << NB_TABLE_SIZE_LOG2)
// the network's input: the levels' features, in float4s
#define NB_INPUT_COUNT (NB_LEVEL_COUNT * NB_FEATURE_COUNT)
#define NB_INPUT4_COUNT (NB_INPUT_COUNT / 4)
#define NB_WIDTH4_COUNT (NB_WIDTH / 4)
// the widest layer's input, in float4s
#define NB_MOST4_COUNT (NB_INPUT4_COUNT > NB_WIDTH4_COUNT ? NB_INPUT4_COUNT : NB_WIDTH4_COUNT)
// the weights: per layer, per four of its outputs, per input, a float4 (that input's weight to each of the four):
// NbLayerFirst(layer) + four * inputCount + input. the last layer's outputs are one four (RGB and an unused one)
#if NB_HIDDEN_COUNT > 0
#define NB_WEIGHT4_COUNT (NB_WIDTH4_COUNT * NB_INPUT_COUNT + (NB_HIDDEN_COUNT - 1) * NB_WIDTH4_COUNT * NB_WIDTH + NB_WIDTH)
#else
#define NB_WEIGHT4_COUNT NB_INPUT_COUNT
#endif
// the samples a thread of ComputeWeightGradient sums: the partial sums are per chunk
#define NB_CHUNK_SAMPLE_COUNT 256
// the grid's gradients are summed in fixed point (atomics add integers alone on every backend): a sample's, clamped
#define NB_GRADIENT_SCALE 65536.0
#define NB_GRADIENT_MOST 64.0

struct NeuralBenchConstants
{
	uint count; // the queries, or the step's samples
	uint seed; // of the queries' or the samples' places
	uint step; // the training steps before this one
	float gridRate; // the learning rates
	float weightRate;
	uint initGrid; // ComputeInit: the grid's entries and the weights' float4s to set
	uint initWeights;
	uint initMeans; // and the means' entries to clear
};

[[vk::binding(0, 0)]]
[[vk::push_constant]]
ConstantBuffer<NeuralBenchConstants> gNeuralBench : register(b0);

struct NeuralBenchSet
{
	// the grid: per level, per entry, its features; their gradients' sums (fixed point) and their moments (Adam's: the
	// mean, the mean square)
	RWStructuredBuffer<float2> gGrid;
	RWStructuredBuffer<int> gGridGradient;
	RWStructuredBuffer<float4> gGridMoments;
	// the network's weights (see NB_WEIGHT4_COUNT), their moments (two float4s each), and their gradients' partial sums
	// (per weight float4, per chunk of samples)
	RWStructuredBuffer<float4> gWeights;
	RWStructuredBuffer<float4> gWeightMoments;
	RWStructuredBuffer<float4> gWeightPartials;
	// a step's samples: per layer input (the features, then each hidden layer's outputs), per float4 of it, per sample;
	// and the loss's gradient at each layer's outputs before the ReLU (the hidden layers', then the last's), likewise
	RWStructuredBuffer<float4> gActivations;
	RWStructuredBuffer<float4> gDeltas;
	// the queries' answers; and ComputeValidate's: per place, the network's and the scene's
	RWStructuredBuffer<float4> gOutput;
	// the baseline without learning (see NbMeanEntry): per level, per cell, the samples' sum (fixed point) and their count
	RWStructuredBuffer<uint> gMeans;
};
[[vk::binding(0, 1)]]
ParameterBlock<NeuralBenchSet> gSetNeuralBench;

uint NbHash(uint value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

float NbRandom(inout uint state)
{
	state = NbHash(state + 0x9e3779b9u);
	return float(state >> 8) * (1.0 / 16777216.0);
}

// a place on a surface of the made up scene: walls an eighth of the cube apart along each axis, as a scene's queries are
// on its surfaces, not all through its volume (so that the fine levels are as sparsely used)
float3 NbPlace(uint index, uint seed)
{
	uint state = NbHash(index ^ NbHash(seed));
	float3 place = float3(NbRandom(state), NbRandom(state), NbRandom(state));
	uint pick = NbHash(state);
	uint axis = pick % 3u;
	place[axis] = (float((pick >> 8) % 8u) + 0.37) / 8.0;
	return place;
}

// the made up scene's radiance: surfaces of a colour each (cells an eighth of the cube wide: edges along the grid's),
// lit smoothly, a shadow's edge across the grid, and small bright lights (a high dynamic range)
float3 NbTarget(float3 place)
{
	uint3 cell = uint3(place * 8.0);
	uint cellHash = NbHash(cell.x + 8u * cell.y + 64u * cell.z + 17u);
	float3 albedo = 0.15 + 0.7 * float3(cellHash & 255u, (cellHash >> 8) & 255u, (cellHash >> 16) & 255u) / 255.0;
	float smooth = 0.5 + 0.5 * sin(dot(place, float3(7.0, 11.0, 13.0)));
	float shadow = place.x + 0.5 * place.y - 0.3 * place.z > 0.7 ? 0.2 : 1.0;
	float3 radiance = albedo * (0.03 + 0.4 * smooth * shadow);
	static const float3 kLights[4] = {float3(0.2, 0.3, 0.6), float3(0.7, 0.8, 0.3), float3(0.5, 0.15, 0.85), float3(0.85, 0.4, 0.5)};
	static const float3 kColors[4] = {float3(20.0, 14.0, 6.0), float3(4.0, 10.0, 18.0), float3(15.0, 3.0, 3.0), float3(8.0, 8.0, 8.0)};
	for (uint lightIt = 0; lightIt < 4; lightIt++)
	{
		float3 to = place - kLights[lightIt];
		radiance += albedo * kColors[lightIt] * exp(-dot(to, to) * 150.0);
	}
	return radiance;
}

uint NbLevelResolution(uint level)
{
	uint resolution = NB_BASE_RESOLUTION * 16u;
	for (uint levelIt = 0; levelIt < level; levelIt++)
		resolution = resolution * NB_GROWTH_16THS / 16u;
	return resolution / 16u;
}

// a level's entry for a corner of its lattice: by its place in the lattice where the lattice fits in the table, else
// hashed (Teschner et al.'s, as Mueller's)
uint NbEntry(uint level, uint resolution, uint3 corner)
{
	uint side = resolution + 1u;
	uint index = side * side * side <= NB_TABLE_SIZE && side < 1024u
		? corner.x + side * (corner.y + side * corner.z)
		: (corner.x ^ (corner.y * 2654435761u) ^ (corner.z * 805459861u)) & (NB_TABLE_SIZE - 1u);
	return level * NB_TABLE_SIZE + index;
}

// the levels' features at a place: each level's interpolated between the corners of its cell
void NbEncode(float3 place, out float4 features[NB_MOST4_COUNT])
{
	for (uint level = 0; level < NB_LEVEL_COUNT; level++)
	{
		uint resolution = NbLevelResolution(level);
		float3 at = place * float(resolution);
		float3 low = min(floor(at), float(resolution - 1u));
		float3 f = at - low;
		float2 sum = 0.0;
		for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		{
			uint3 offset = uint3(cornerIt & 1u, (cornerIt >> 1) & 1u, cornerIt >> 2);
			float3 w = lerp(1.0 - f, f, float3(offset));
			sum += w.x * w.y * w.z * gSetNeuralBench.gGrid[NbEntry(level, resolution, uint3(low) + offset)];
		}
		if ((level & 1u) == 0u)
			features[level / 2u].xy = sum;
		else
			features[level / 2u].zw = sum;
	}
}

// a sample's share of the loss's gradient at the levels' features, to the entries they were interpolated from
void NbEncodeGradient(float3 place, float4 gradient[NB_MOST4_COUNT])
{
	for (uint level = 0; level < NB_LEVEL_COUNT; level++)
	{
		uint resolution = NbLevelResolution(level);
		float3 at = place * float(resolution);
		float3 low = min(floor(at), float(resolution - 1u));
		float3 f = at - low;
		float2 g = (level & 1u) == 0u ? gradient[level / 2u].xy : gradient[level / 2u].zw;
		g = clamp(g, -NB_GRADIENT_MOST, NB_GRADIENT_MOST) * NB_GRADIENT_SCALE;
		for (uint cornerIt = 0; cornerIt < 8; cornerIt++)
		{
			uint3 offset = uint3(cornerIt & 1u, (cornerIt >> 1) & 1u, cornerIt >> 2);
			float3 w = lerp(1.0 - f, f, float3(offset));
			int2 amount = int2(round(w.x * w.y * w.z * g));
			uint entry = NbEntry(level, resolution, uint3(low) + offset);
			if (amount.x != 0)
				InterlockedAdd(gSetNeuralBench.gGridGradient[entry * 2u], amount.x);
			if (amount.y != 0)
				InterlockedAdd(gSetNeuralBench.gGridGradient[entry * 2u + 1u], amount.y);
		}
	}
}

uint NbLayerInputCount(uint layer)
{
	return layer == 0u ? NB_INPUT_COUNT : NB_WIDTH;
}

// a layer's outputs, in float4s: the hidden layers', and the last's one
uint NbLayerOutput4Count(uint layer)
{
	return layer < NB_HIDDEN_COUNT ? NB_WIDTH4_COUNT : 1u;
}

uint NbLayerFirst(uint layer)
{
	uint first = 0;
	for (uint layerIt = 0; layerIt < layer; layerIt++)
		first += NbLayerOutput4Count(layerIt) * NbLayerInputCount(layerIt);
	return first;
}

// where a layer's inputs are in gActivations, and its outputs' gradients in gDeltas, in float4s per sample
uint NbActivationFirst(uint layer)
{
	return layer == 0u ? 0u : NB_INPUT4_COUNT + (layer - 1u) * NB_WIDTH4_COUNT;
}
#define NB_ACTIVATION4_COUNT (NB_INPUT4_COUNT + NB_HIDDEN_COUNT * NB_WIDTH4_COUNT)
#define NB_DELTA4_COUNT (NB_HIDDEN_COUNT * NB_WIDTH4_COUNT + 1)

// a layer's outputs of its inputs, before the ReLU
void NbLayer(uint layer, float4 input[NB_MOST4_COUNT], out float4 output[NB_MOST4_COUNT])
{
	uint first = NbLayerFirst(layer);
	uint inputCount = NbLayerInputCount(layer);
	uint outputCount = NbLayerOutput4Count(layer);
	for (uint outputIt = 0; outputIt < outputCount; outputIt++)
	{
		float4 sum = 0.0;
		uint row = first + outputIt * inputCount;
		for (uint inputIt = 0; inputIt < inputCount / 4u; inputIt++)
		{
			float4 value = input[inputIt];
			sum += gSetNeuralBench.gWeights[row + inputIt * 4u] * value.x;
			sum += gSetNeuralBench.gWeights[row + inputIt * 4u + 1u] * value.y;
			sum += gSetNeuralBench.gWeights[row + inputIt * 4u + 2u] * value.z;
			sum += gSetNeuralBench.gWeights[row + inputIt * 4u + 3u] * value.w;
		}
		output[outputIt] = sum;
	}
}

// the network's answer at a place
float3 NbQuery(float3 place)
{
	float4 values[NB_MOST4_COUNT];
	NbEncode(place, values);
	for (uint layer = 0; layer < NB_HIDDEN_COUNT; layer++)
	{
		float4 next[NB_MOST4_COUNT];
		NbLayer(layer, values, next);
		for (uint valueIt = 0; valueIt < NB_WIDTH4_COUNT; valueIt++)
			values[valueIt] = max(next[valueIt], 0.0);
	}
	float4 last[NB_MOST4_COUNT];
	NbLayer(NB_HIDDEN_COUNT, values, last);
	return last[0].xyz;
}

// the grid's features small and random, the weights uniform within what keeps a layer's outputs as large as its inputs
// (Glorot's), the moments and the gradients zero
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeInit(uint3 dispatchId: SV_DispatchThreadID)
{
	uint index = dispatchId.x;
	uint state = NbHash(index ^ 0x51ed270bu);
	if (index < gNeuralBench.initGrid)
	{
		gSetNeuralBench.gGrid[index] = (float2(NbRandom(state), NbRandom(state)) * 2.0 - 1.0) * 1e-4;
		gSetNeuralBench.gGridGradient[index * 2u] = 0;
		gSetNeuralBench.gGridGradient[index * 2u + 1u] = 0;
		gSetNeuralBench.gGridMoments[index] = 0.0;
	}
	if (index < gNeuralBench.initWeights)
	{
		uint layer = 0;
		while (layer < NB_HIDDEN_COUNT && index >= NbLayerFirst(layer + 1u))
			layer++;
		float reach = sqrt(6.0 / float(NbLayerInputCount(layer) + (layer < NB_HIDDEN_COUNT ? NB_WIDTH : 3)));
		gSetNeuralBench.gWeights[index] = (float4(NbRandom(state), NbRandom(state), NbRandom(state), NbRandom(state)) * 2.0 - 1.0) * reach;
		gSetNeuralBench.gWeightMoments[index * 2u] = 0.0;
		gSetNeuralBench.gWeightMoments[index * 2u + 1u] = 0.0;
	}
	if (index < gNeuralBench.initMeans)
		for (uint channel = 0; channel < 4; channel++)
			gSetNeuralBench.gMeans[index * 4u + channel] = 0u;
}

// a query per thread: its answer stored
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeInfer(uint3 dispatchId: SV_DispatchThreadID)
{
	if (dispatchId.x >= gNeuralBench.count)
		return;
	gSetNeuralBench.gOutput[dispatchId.x] = float4(NbQuery(NbPlace(dispatchId.x, gNeuralBench.seed)), 1.0);
}

// the network's answer and the scene's at a place per thread, for the host to compare
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeValidate(uint3 dispatchId: SV_DispatchThreadID)
{
	if (dispatchId.x >= gNeuralBench.count)
		return;
	float3 place = NbPlace(dispatchId.x, gNeuralBench.seed);
	gSetNeuralBench.gOutput[dispatchId.x * 2u] = float4(NbQuery(place), 1.0);
	gSetNeuralBench.gOutput[dispatchId.x * 2u + 1u] = float4(NbTarget(place), 1.0);
}

// a training step's first pass, a sample per thread: the network's answer at a place, each layer's inputs kept
// (gActivations); the loss against a noisy estimate of the scene's radiance there (as a path's: its mean is the
// radiance), relative to the answer (Mueller et al. 2021's: the square of the difference over the answer's square, which
// weighs the dark as much as the bright); its gradient back through the layers (gDeltas: at each layer's outputs) and
// through the features to the grid's entries (summed there with atomics). the weights' gradients are the next pass's:
// summed per sample with atomics, as the grid's are and as Slang's neural module sums them, every sample's thread would
// add to every weight
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeTrain(uint3 dispatchId: SV_DispatchThreadID)
{
	uint sample = dispatchId.x;
	uint count = gNeuralBench.count;
	if (sample >= count)
		return;
	float3 place = NbPlace(sample, gNeuralBench.seed);
	uint state = NbHash(sample ^ NbHash(gNeuralBench.seed + 0x2545f491u));
	float3 estimate = NbTarget(place) * (2.0 * NbRandom(state));

	float4 values[NB_MOST4_COUNT];
	NbEncode(place, values);
	for (uint valueIt = 0; valueIt < NB_INPUT4_COUNT; valueIt++)
		gSetNeuralBench.gActivations[valueIt * count + sample] = values[valueIt];
	for (uint layer = 0; layer < NB_HIDDEN_COUNT; layer++)
	{
		float4 next[NB_MOST4_COUNT];
		NbLayer(layer, values, next);
		uint first = NbActivationFirst(layer + 1u);
		for (uint valueIt = 0; valueIt < NB_WIDTH4_COUNT; valueIt++)
		{
			values[valueIt] = max(next[valueIt], 0.0);
			gSetNeuralBench.gActivations[(first + valueIt) * count + sample] = values[valueIt];
		}
	}
	float4 last[NB_MOST4_COUNT];
	NbLayer(NB_HIDDEN_COUNT, values, last);
	float3 answer = last[0].xyz;

	// the gradient at the last layer's outputs, then at each layer's before it: its outputs' gradients through its
	// weights to its inputs, which are the layer before's outputs after the ReLU (nothing where that cut them off)
	float4 delta[NB_MOST4_COUNT];
	delta[0] = float4(clamp(2.0 * (answer - estimate) / (answer * answer + 0.01), -16.0, 16.0), 0.0);
	gSetNeuralBench.gDeltas[(NB_DELTA4_COUNT - 1u) * count + sample] = delta[0];
	for (int layerIt = NB_HIDDEN_COUNT; layerIt >= 0; layerIt--)
	{
		uint layer = uint(layerIt);
		uint first = NbLayerFirst(layer);
		uint inputCount = NbLayerInputCount(layer);
		uint outputCount = NbLayerOutput4Count(layer);
		float4 inputs[NB_MOST4_COUNT];
		for (uint inputIt = 0; inputIt < inputCount / 4u; inputIt++)
		{
			float4 sum = 0.0;
			for (uint outputIt = 0; outputIt < outputCount; outputIt++)
			{
				uint row = first + outputIt * inputCount + inputIt * 4u;
				float4 d = delta[outputIt];
				sum.x += dot(gSetNeuralBench.gWeights[row], d);
				sum.y += dot(gSetNeuralBench.gWeights[row + 1u], d);
				sum.z += dot(gSetNeuralBench.gWeights[row + 2u], d);
				sum.w += dot(gSetNeuralBench.gWeights[row + 3u], d);
			}
			inputs[inputIt] = sum;
		}
		if (layer == 0u)
		{
			NbEncodeGradient(place, inputs);
			break;
		}
		uint activations = NbActivationFirst(layer);
		for (uint inputIt = 0; inputIt < NB_WIDTH4_COUNT; inputIt++)
		{
			float4 activation = gSetNeuralBench.gActivations[(activations + inputIt) * count + sample];
			delta[inputIt] = float4(activation.x > 0.0 ? inputs[inputIt].x : 0.0, activation.y > 0.0 ? inputs[inputIt].y : 0.0,
									activation.z > 0.0 ? inputs[inputIt].z : 0.0, activation.w > 0.0 ? inputs[inputIt].w : 0.0);
			gSetNeuralBench.gDeltas[((layer - 1u) * NB_WIDTH4_COUNT + inputIt) * count + sample] = delta[inputIt];
		}
	}
}

// a training step's second pass: the weights' gradients, each the sum over the samples of its output's gradient times
// its input (a matrix product of the samples' gradients and inputs: what tensor cores do). a thread per four outputs,
// four inputs and chunk of samples sums its sixteen weights' over the chunk (gWeightPartials). dispatched over
// (the weights' float4s over 4, the chunks)
[shader("compute")]
[numthreads(16, 4, 1)]
void ComputeWeightGradient(uint3 dispatchId: SV_DispatchThreadID)
{
	uint count = gNeuralBench.count;
	uint chunkCount = (count + NB_CHUNK_SAMPLE_COUNT - 1u) / NB_CHUNK_SAMPLE_COUNT;
	uint block = dispatchId.x; // four weight float4s: four inputs to four outputs
	uint chunk = dispatchId.y;
	if (block >= NB_WEIGHT4_COUNT / 4u || chunk >= chunkCount)
		return;
	uint layer = 0;
	while (layer < NB_HIDDEN_COUNT && block * 4u >= NbLayerFirst(layer + 1u))
		layer++;
	uint within = block * 4u - NbLayerFirst(layer);
	uint inputCount = NbLayerInputCount(layer);
	uint output4 = within / inputCount;
	uint input4 = (within % inputCount) / 4u;
	uint activations = (NbActivationFirst(layer) + input4) * count;
	uint deltas = ((layer < NB_HIDDEN_COUNT ? layer * NB_WIDTH4_COUNT : NB_DELTA4_COUNT - 1u) + output4) * count;
	float4 sums[4] = {float4(0.0), float4(0.0), float4(0.0), float4(0.0)};
	uint end = min((chunk + 1u) * NB_CHUNK_SAMPLE_COUNT, count);
	for (uint sample = chunk * NB_CHUNK_SAMPLE_COUNT; sample < end; sample++)
	{
		float4 activation = gSetNeuralBench.gActivations[activations + sample];
		float4 delta = gSetNeuralBench.gDeltas[deltas + sample];
		sums[0] += delta * activation.x;
		sums[1] += delta * activation.y;
		sums[2] += delta * activation.z;
		sums[3] += delta * activation.w;
	}
	for (uint weightIt = 0; weightIt < 4; weightIt++)
		gSetNeuralBench.gWeightPartials[(block * 4u + weightIt) * chunkCount + chunk] = sums[weightIt];
}

// Adam's step (Kingma and Ba's, as Mueller's: 0.9 and 0.99 of the moments kept), on a value and its gradient
float4 NbAdam(float4 value, float4 gradient, inout float4 mean, inout float4 meanSquare, float rate, uint step)
{
	mean = lerp(gradient, mean, 0.9);
	meanSquare = lerp(gradient * gradient, meanSquare, 0.99);
	float4 meanNow = mean / (1.0 - pow(0.9, float(step + 1u)));
	float4 meanSquareNow = meanSquare / (1.0 - pow(0.99, float(step + 1u)));
	return value - rate * meanNow / (sqrt(meanSquareNow) + 1e-15);
}

// a training step's third pass, a weight float4 per thread: its gradient summed over the chunks, and its step
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeWeightStep(uint3 dispatchId: SV_DispatchThreadID)
{
	uint index = dispatchId.x;
	if (index >= NB_WEIGHT4_COUNT)
		return;
	uint chunkCount = (gNeuralBench.count + NB_CHUNK_SAMPLE_COUNT - 1u) / NB_CHUNK_SAMPLE_COUNT;
	float4 gradient = 0.0;
	for (uint chunk = 0; chunk < chunkCount; chunk++)
		gradient += gSetNeuralBench.gWeightPartials[index * chunkCount + chunk];
	float4 mean = gSetNeuralBench.gWeightMoments[index * 2u];
	float4 meanSquare = gSetNeuralBench.gWeightMoments[index * 2u + 1u];
	gSetNeuralBench.gWeights[index] = NbAdam(gSetNeuralBench.gWeights[index], gradient, mean, meanSquare, gNeuralBench.weightRate, gNeuralBench.step);
	gSetNeuralBench.gWeightMoments[index * 2u] = mean;
	gSetNeuralBench.gWeightMoments[index * 2u + 1u] = meanSquare;
}

// a training step's last pass, a grid entry per thread: its step, where a sample reached it (the others keep their
// moments: an entry no sample touches doesn't drift, as Mueller's), and its gradients' sums cleared for the next step
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeGridStep(uint3 dispatchId: SV_DispatchThreadID)
{
	uint index = dispatchId.x;
	if (index >= NB_LEVEL_COUNT * NB_TABLE_SIZE)
		return;
	int2 sums = int2(gSetNeuralBench.gGridGradient[index * 2u], gSetNeuralBench.gGridGradient[index * 2u + 1u]);
	if (sums.x == 0 && sums.y == 0)
		return;
	gSetNeuralBench.gGridGradient[index * 2u] = 0;
	gSetNeuralBench.gGridGradient[index * 2u + 1u] = 0;
	float4 moments = gSetNeuralBench.gGridMoments[index];
	float4 mean = float4(moments.xy, 0.0, 0.0);
	float4 meanSquare = float4(moments.zw, 0.0, 0.0);
	float4 gradient = float4(float2(sums) / NB_GRADIENT_SCALE, 0.0, 0.0);
	float4 value = NbAdam(float4(gSetNeuralBench.gGrid[index], 0.0, 0.0), gradient, mean, meanSquare, gNeuralBench.gridRate, gNeuralBench.step);
	gSetNeuralBench.gGrid[index] = value.xy;
	gSetNeuralBench.gGridMoments[index] = float4(mean.xy, meanSquare.xy);
}

// the baseline without learning, as the hashed radiance caches keep theirs (a cell's light the mean of the samples that
// fell in it): NB_MEAN_LEVEL_COUNT grids of cells, 16 a side and each twice the last's, every cell an entry of its own
// (no hashing: the finest is 128 a side). a query takes the finest level's cell that holds NB_MEAN_SAMPLE_COUNT_LEAST
// samples, else the coarsest's
#define NB_MEAN_LEVEL_COUNT 4
#define NB_MEAN_SCALE 256.0
#define NB_MEAN_SAMPLE_COUNT_LEAST 16u

uint NbMeanEntry(uint level, float3 place)
{
	uint resolution = 16u << level;
	uint3 cell = min(uint3(place * float(resolution)), resolution - 1u);
	// the levels before: 16^3 (1 + 8 + ...)
	uint first = 4096u * ((1u << (3u * level)) - 1u) / 7u;
	return first + cell.x + resolution * (cell.y + resolution * cell.z);
}

float3 NbMeanQuery(float3 place)
{
	for (int level = NB_MEAN_LEVEL_COUNT - 1; level >= 0; level--)
	{
		uint entry = NbMeanEntry(uint(level), place) * 4u;
		uint count = gSetNeuralBench.gMeans[entry + 3u];
		if (count >= NB_MEAN_SAMPLE_COUNT_LEAST || (level == 0 && count > 0u))
			return float3(gSetNeuralBench.gMeans[entry], gSetNeuralBench.gMeans[entry + 1u], gSetNeuralBench.gMeans[entry + 2u]) / (NB_MEAN_SCALE * float(count));
	}
	return 0.0;
}

// a step's samples (ComputeTrain's places and estimates) added to their cells
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMeanAdd(uint3 dispatchId: SV_DispatchThreadID)
{
	uint sample = dispatchId.x;
	if (sample >= gNeuralBench.count)
		return;
	float3 place = NbPlace(sample, gNeuralBench.seed);
	uint state = NbHash(sample ^ NbHash(gNeuralBench.seed + 0x2545f491u));
	uint3 estimate = uint3(NbTarget(place) * (2.0 * NbRandom(state)) * NB_MEAN_SCALE + 0.5);
	for (uint level = 0; level < NB_MEAN_LEVEL_COUNT; level++)
	{
		uint entry = NbMeanEntry(level, place) * 4u;
		InterlockedAdd(gSetNeuralBench.gMeans[entry], estimate.x);
		InterlockedAdd(gSetNeuralBench.gMeans[entry + 1u], estimate.y);
		InterlockedAdd(gSetNeuralBench.gMeans[entry + 2u], estimate.z);
		InterlockedAdd(gSetNeuralBench.gMeans[entry + 3u], 1u);
	}
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMeanInfer(uint3 dispatchId: SV_DispatchThreadID)
{
	if (dispatchId.x >= gNeuralBench.count)
		return;
	gSetNeuralBench.gOutput[dispatchId.x] = float4(NbMeanQuery(NbPlace(dispatchId.x, gNeuralBench.seed)), 1.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMeanValidate(uint3 dispatchId: SV_DispatchThreadID)
{
	if (dispatchId.x >= gNeuralBench.count)
		return;
	float3 place = NbPlace(dispatchId.x, gNeuralBench.seed);
	gSetNeuralBench.gOutput[dispatchId.x * 2u] = float4(NbMeanQuery(place), 1.0);
	gSetNeuralBench.gOutput[dispatchId.x * 2u + 1u] = float4(NbTarget(place), 1.0);
}
