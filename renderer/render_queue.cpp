#include "render_queue.hpp"
#include "render_context.hpp"
#include <cstring>
#include <iterator>
#include <algorithm>
#include <assert.h>

using namespace std;
using namespace Vulkan;
using namespace Util;

namespace Granite
{
void RenderQueue::sort()
{
	for (auto &queue : queues)
	{
		stable_sort(queue.queue, queue.queue + queue.count, [](const RenderInfo *a, const RenderInfo *b) {
			return a->sorting_key < b->sorting_key;
		});
	}
}

void RenderQueue::combine_render_info(const RenderQueue &queue)
{
	for (unsigned i = 0; i < ecast(Queue::Count); i++)
	{
		auto e = static_cast<Queue>(i);
		size_t n = queue.get_queue_count(e);
		auto **other_infos = queue.get_queue(e);
		for (size_t i = 0; i < n; i++)
			enqueue(e, other_infos[i]);
	}
}

void RenderQueue::dispatch(Queue queue_type, CommandBuffer &cmd, const CommandBufferSavedState *state, size_t begin, size_t end)
{
	auto *queue = queues[ecast(queue_type)].queue;

	while (begin < end)
	{
		assert(queue[begin]->instance_key != 0);
		assert(queue[begin]->sorting_key != 0);

		if (state)
			cmd.restore_state(*state);

		unsigned instances = 1;
		for (size_t i = begin + 1; i < end && queue[i]->instance_key == queue[begin]->instance_key; i++)
		{
			assert(queue[i]->render == queue[begin]->render);
			instances++;
		}

		queue[begin]->render(cmd, &queue[begin], instances);
		begin += instances;
	}
}

void RenderQueue::dispatch(Queue queue, CommandBuffer &cmd, const CommandBufferSavedState *state)
{
	dispatch(queue, cmd, state, 0, queues[ecast(queue)].count);
}

void RenderQueue::enqueue(Queue queue_type, const RenderInfo *render_info)
{
	auto &info = queues[ecast(queue_type)];

	if (info.count >= info.capacity)
	{
		size_t new_capacity = info.capacity ? info.capacity * 2 : 64;
		const RenderInfo **new_queue = static_cast<const RenderInfo **>(allocate(sizeof(RenderInfo *) * new_capacity, alignof(RenderInfo *)));
		memcpy(new_queue, info.queue, info.count * sizeof(*new_queue));
		info.queue = new_queue;
		info.capacity = new_capacity;
	}
	info.queue[info.count++] = render_info;
}

RenderQueue::Chain::iterator RenderQueue::insert_block()
{
	return blocks.insert(end(blocks), Block(BlockSize));
}

void *RenderQueue::allocate_from_block(Block &block, size_t size, size_t alignment)
{
	block.ptr = (block.ptr + alignment - 1) & ~(alignment - 1);
	uintptr_t end = block.ptr + size;
	if (end <= block.end)
	{
		void *ret = reinterpret_cast<void *>(block.ptr);
		block.ptr = end;
		return ret;
	}
	else
		return nullptr;
}

void RenderQueue::reset()
{
	current = begin(blocks);
	if (current != end(blocks))
		current->reset();

	memset(queues, 0, sizeof(queues));
}

void RenderQueue::reset_and_reclaim()
{
	blocks.clear();
	current = end(blocks);

	memset(queues, 0, sizeof(queues));
}

void *RenderQueue::allocate(size_t size, size_t alignment)
{
	if (size + alignment > BlockSize)
		return nullptr;

	// First allocation.
	if (current == end(blocks))
		current = insert_block();

	void *data = allocate_from_block(*current, size, alignment);
	if (data)
		return data;

	++current;
	if (current == end(blocks))
		current = insert_block();
	else
		current->reset();

	data = allocate_from_block(*current, size, alignment);
	return data;
}

uint64_t RenderInfo::get_background_sort_key(Queue queue_type, Util::Hash pipeline_hash)
{
	if (queue_type == Queue::Transparent)
		return pipeline_hash & 0xffffffffu;
	else
		return (UINT64_MAX << 32) | (pipeline_hash & 0xffffffffu);
}

uint64_t RenderInfo::get_sprite_sort_key(Queue queue_type, Util::Hash pipeline_hash, float z, StaticLayer layer)
{
	static_assert(ecast(StaticLayer::Count) == 4, "Number of static layers is not 4.");

	// Monotonically increasing floating point will be monotonic in uint32_t as well when z is non-negative.
	z = glm::max(z, 0.0f);
	uint32_t depth_key = floatBitsToUint(z);

	if (queue_type == Queue::Transparent)
	{
		depth_key ^= 0xffffffffu; // Back-to-front instead.
		// Prioritize correct back-to-front rendering over pipeline.
		return (Hash(depth_key) << 32) | (pipeline_hash & 0xffffffffu);
	}
	else
	{
		depth_key >>= 2;
		pipeline_hash &= 0xffffffffu;

		// Prioritize state changes over depth.
		return (uint64_t(ecast(layer)) << 62) | (pipeline_hash << 30) | depth_key;
	}
}

uint64_t RenderInfo::get_sort_key(const RenderContext &context, Queue queue_type, Util::Hash pipeline_hash,
                                  const vec3 &center, StaticLayer layer)
{
	float z = dot(context.get_render_parameters().camera_front, center - context.get_render_parameters().camera_position);
	return get_sprite_sort_key(queue_type, pipeline_hash, z, layer);
}
}