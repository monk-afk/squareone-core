/*
Minetest
Copyright (C) 2026 SquareOne contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 3.0 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#pragma once

#include "irrlichttypes_bloated.h"
#include "staticobject.h"
#include <deque>
#include <map>
#include <utility>

/*
	Round-robin scheduler for stored active-object activation.

	The queue deliberately owns no StaticObjects or MapBlocks. StaticObjects stay
	in their MapBlock's m_stored vector until the activation callback is run, and
	MapBlocks are looked up from the queued position on every visit.
*/
class ActiveObjectActivationQueue
{
public:
	struct Entry
	{
		v3s16 blockpos;
		u32 dtime_s;
		u32 objects_left;
		u64 serial;
	};

	enum class Result
	{
		CONSUMED,
		CANCELLED,
	};

	bool enqueue(v3s16 blockpos, u32 dtime_s, u32 object_count)
	{
		if (object_count == 0 || m_block_serials.count(blockpos) != 0)
			return false;

		if (++m_next_serial == 0)
			++m_next_serial;

		Entry entry{blockpos, dtime_s, object_count, m_next_serial};
		m_queue.push_back(entry);
		m_block_serials[blockpos] = entry.serial;
		m_pending_objects += object_count;
		return true;
	}

	bool cancel(v3s16 blockpos)
	{
		auto serial_it = m_block_serials.find(blockpos);
		if (serial_it == m_block_serials.end())
			return false;

		const u64 serial = serial_it->second;
		m_block_serials.erase(serial_it);
		if (m_inflight_serial == serial) {
			m_pending_objects -= m_inflight_objects_left;
			m_inflight_serial = 0;
			m_inflight_objects_left = 0;
			return true;
		}

		for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
			if (it->serial != serial)
				continue;

			m_pending_objects -= it->objects_left;
			m_queue.erase(it);
			break;
		}
		return true;
	}

	void clear()
	{
		m_queue.clear();
		m_block_serials.clear();
		m_pending_objects = 0;
		m_inflight_serial = 0;
		m_inflight_objects_left = 0;
	}

	template <typename ActivationCallback, typename TimeCallback>
	u32 process(u32 max_activations, u64 time_budget_us,
			const ActivationCallback &activate, const TimeCallback &get_time)
	{
		const u64 start_time = get_time();
		u32 activation_count = 0;
		u32 callback_count = 0;

		while (!m_queue.empty() &&
				(max_activations == 0 ||
						activation_count < max_activations)) {
			if (callback_count != 0 && time_budget_us != 0 &&
					get_time() - start_time >= time_budget_us)
				break;

			Entry entry = m_queue.front();
			m_queue.pop_front();

			auto serial_it = m_block_serials.find(entry.blockpos);
			if (serial_it == m_block_serials.end() ||
					serial_it->second != entry.serial)
				continue;

			// Account for this object before running Lua. The callback may
			// re-enter this queue through clearObjects().
			--entry.objects_left;
			--m_pending_objects;
			m_inflight_serial = entry.serial;
			m_inflight_objects_left = entry.objects_left;
			const Result result = activate(entry);
			m_inflight_serial = 0;
			m_inflight_objects_left = 0;
			++callback_count;

			serial_it = m_block_serials.find(entry.blockpos);
			const bool still_current = serial_it != m_block_serials.end() &&
						   serial_it->second == entry.serial;

			if (result == Result::CANCELLED) {
				if (still_current) {
					m_pending_objects -= entry.objects_left;
					m_block_serials.erase(serial_it);
				}
				continue;
			}

			++activation_count;
			if (!still_current)
				continue;

			if (entry.objects_left != 0)
				m_queue.push_back(entry);
			else
				m_block_serials.erase(serial_it);
		}

		return activation_count;
	}

	size_t pendingBlockCount() const { return m_block_serials.size(); }
	u64 pendingObjectCount() const { return m_pending_objects; }
	bool empty() const { return m_block_serials.empty(); }

private:
	std::deque<Entry> m_queue;
	std::map<v3s16, u64> m_block_serials;
	u64 m_pending_objects = 0;
	u64 m_next_serial = 0;
	u64 m_inflight_serial = 0;
	u32 m_inflight_objects_left = 0;
};

/*
	Remove one stored object before activation and restore it if activation
	fails. Appending failures allows the rest of the original block snapshot to
	be attempted without retrying one bad object indefinitely.
*/
template <typename ActivationCallback>
bool activateNextStoredObject(
		std::vector<StaticObject> &stored, const ActivationCallback &activate)
{
	if (stored.empty())
		return false;

	StaticObject object = std::move(stored.front());
	stored.erase(stored.begin());
	if (!activate(object))
		stored.push_back(std::move(object));
	return true;
}
