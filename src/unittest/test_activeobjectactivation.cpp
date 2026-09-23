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

#include "activeobject.h"
#include "server/active_object_activation_queue.h"
#include "test.h"
#include <sstream>
#include <stdexcept>

class TestActiveObjectActivation : public TestBase
{
public:
	TestActiveObjectActivation() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestActiveObjectActivation"; }

	void runTests(IGameDef *gamedef);

	void testBoundedAcrossSteps();
	void testLegacyAllAtOnce();
	void testRoundRobin();
	void testCancellationPreservesStoredObjects();
	void testFailureRestoresStaticObject();
	void testTimeBudget();
	void testReentrantCancellation();
};

static TestActiveObjectActivation g_test_instance;

void TestActiveObjectActivation::runTests(IGameDef *gamedef)
{
	TEST(testBoundedAcrossSteps);
	TEST(testLegacyAllAtOnce);
	TEST(testRoundRobin);
	TEST(testCancellationPreservesStoredObjects);
	TEST(testFailureRestoresStaticObject);
	TEST(testTimeBudget);
	TEST(testReentrantCancellation);
}

static StaticObject makeStaticObject(const std::string &data)
{
	StaticObject object;
	object.type = ACTIVEOBJECT_TYPE_LUAENTITY;
	object.data = data;
	return object;
}

static size_t serializedObjectCount(StaticObjectList &objects)
{
	std::ostringstream os(std::ios::binary);
	objects.serialize(os);
	std::istringstream is(os.str(), std::ios::binary);
	StaticObjectList copy;
	copy.deSerialize(is);
	return copy.m_stored.size();
}

void TestActiveObjectActivation::testBoundedAcrossSteps()
{
	StaticObjectList objects;
	objects.m_stored.push_back(makeStaticObject("one"));
	objects.m_stored.push_back(makeStaticObject("two"));
	objects.m_stored.push_back(makeStaticObject("three"));

	ActiveObjectActivationQueue queue;
	const v3s16 pos(1, 2, 3);
	UASSERT(queue.enqueue(pos, 42, objects.m_stored.size()));
	UASSERTCMP(size_t, ==, serializedObjectCount(objects), 3);

	std::vector<std::string> activated;
	u16 active_id = 1;
	auto activate = [&](const ActiveObjectActivationQueue::Entry &entry) {
		UASSERT(entry.blockpos == pos);
		UASSERTCMP(u32, ==, entry.dtime_s, 42);
		UASSERT(activateNextStoredObject(
				objects.m_stored, [&](const StaticObject &object) {
					activated.push_back(object.data);
					objects.m_active[active_id++] = object;
					return true;
				}));
		return ActiveObjectActivationQueue::Result::CONSUMED;
	};
	auto now = []() { return (u64)0; };

	UASSERTCMP(u32, ==, queue.process(1, 0, activate, now), 1);
	UASSERTCMP(size_t, ==, objects.m_stored.size(), 2);
	UASSERTCMP(size_t, ==, serializedObjectCount(objects), 3);
	UASSERTCMP(u64, ==, queue.pendingObjectCount(), 2);
	UASSERTCMP(u32, ==, queue.process(1, 0, activate, now), 1);
	UASSERTCMP(size_t, ==, objects.m_stored.size(), 1);
	UASSERTCMP(u32, ==, queue.process(1, 0, activate, now), 1);
	UASSERT(objects.m_stored.empty());
	UASSERTCMP(size_t, ==, serializedObjectCount(objects), 3);
	UASSERT(queue.empty());
	UASSERTCMP(size_t, ==, activated.size(), 3);
	UASSERT(activated[0] == "one" && activated[1] == "two" &&
			activated[2] == "three");
}

void TestActiveObjectActivation::testLegacyAllAtOnce()
{
	std::vector<StaticObject> stored;
	stored.push_back(makeStaticObject("one"));
	stored.push_back(makeStaticObject("two"));
	stored.push_back(makeStaticObject("three"));

	const u32 original_count = stored.size();
	u32 activated = 0;
	for (u32 i = 0; i < original_count; ++i) {
		UASSERT(activateNextStoredObject(stored, [&](const StaticObject &object) {
			++activated;
			return true;
		}));
	}

	UASSERTCMP(u32, ==, activated, 3);
	UASSERT(stored.empty());
}

void TestActiveObjectActivation::testRoundRobin()
{
	ActiveObjectActivationQueue queue;
	const v3s16 first(1, 0, 0);
	const v3s16 second(2, 0, 0);
	UASSERT(queue.enqueue(first, 10, 3));
	UASSERT(queue.enqueue(second, 20, 2));

	std::vector<v3s16> order;
	auto activate = [&](const ActiveObjectActivationQueue::Entry &entry) {
		order.push_back(entry.blockpos);
		return ActiveObjectActivationQueue::Result::CONSUMED;
	};
	auto now = []() { return (u64)0; };

	UASSERTCMP(u32, ==, queue.process(4, 0, activate, now), 4);
	UASSERTCMP(size_t, ==, order.size(), 4);
	UASSERT(order[0] == first && order[1] == second && order[2] == first &&
			order[3] == second);
	UASSERTCMP(size_t, ==, queue.pendingBlockCount(), 1);
	UASSERTCMP(u64, ==, queue.pendingObjectCount(), 1);
}

void TestActiveObjectActivation::testCancellationPreservesStoredObjects()
{
	StaticObjectList objects;
	objects.m_stored.push_back(makeStaticObject("one"));
	objects.m_stored.push_back(makeStaticObject("two"));

	ActiveObjectActivationQueue queue;
	const v3s16 pos(1, 2, 3);
	UASSERT(queue.enqueue(pos, 0, objects.m_stored.size()));
	UASSERT(queue.cancel(pos));
	UASSERT(queue.empty());
	UASSERTCMP(size_t, ==, objects.m_stored.size(), 2);
	UASSERTCMP(size_t, ==, serializedObjectCount(objects), 2);

	UASSERT(queue.enqueue(pos, 0, objects.m_stored.size()));
	u32 callbacks = 0;
	const u32 activated = queue.process(
			4, 0,
			[&](const ActiveObjectActivationQueue::Entry &entry) {
				++callbacks;
				return ActiveObjectActivationQueue::Result::CANCELLED;
			},
			[]() { return (u64)0; });
	UASSERTCMP(u32, ==, activated, 0);
	UASSERTCMP(u32, ==, callbacks, 1);
	UASSERT(queue.empty());
	UASSERTCMP(size_t, ==, objects.m_stored.size(), 2);
}

void TestActiveObjectActivation::testFailureRestoresStaticObject()
{
	StaticObjectList objects;
	objects.m_stored.push_back(makeStaticObject("bad"));
	objects.m_stored.push_back(makeStaticObject("good"));

	ActiveObjectActivationQueue queue;
	UASSERT(queue.enqueue(v3s16(), 0, objects.m_stored.size()));
	u32 callbacks = 0;
	auto activate = [&](const ActiveObjectActivationQueue::Entry &entry) {
		UASSERT(activateNextStoredObject(
				objects.m_stored, [&](const StaticObject &object) {
					++callbacks;
					return object.data != "bad";
				}));
		return ActiveObjectActivationQueue::Result::CONSUMED;
	};
	auto now = []() { return (u64)0; };

	UASSERTCMP(u32, ==, queue.process(1, 0, activate, now), 1);
	UASSERTCMP(size_t, ==, objects.m_stored.size(), 2);
	UASSERT(objects.m_stored[0].data == "good");
	UASSERT(objects.m_stored[1].data == "bad");
	UASSERTCMP(u32, ==, queue.process(1, 0, activate, now), 1);
	UASSERT(queue.empty());
	UASSERTCMP(size_t, ==, objects.m_stored.size(), 1);
	UASSERT(objects.m_stored[0].data == "bad");
	UASSERTCMP(size_t, ==, serializedObjectCount(objects), 1);
	UASSERTCMP(u32, ==, callbacks, 2);
}

void TestActiveObjectActivation::testTimeBudget()
{
	ActiveObjectActivationQueue queue;
	UASSERT(queue.enqueue(v3s16(), 0, 3));
	u64 now_us = 0;
	u32 callbacks = 0;
	const u32 activated = queue.process(
			0, 100,
			[&](const ActiveObjectActivationQueue::Entry &entry) {
				++callbacks;
				now_us += 100;
				return ActiveObjectActivationQueue::Result::CONSUMED;
			},
			[&]() { return now_us; });

	UASSERTCMP(u32, ==, activated, 1);
	UASSERTCMP(u32, ==, callbacks, 1);
	UASSERTCMP(u64, ==, queue.pendingObjectCount(), 2);
}

void TestActiveObjectActivation::testReentrantCancellation()
{
	ActiveObjectActivationQueue queue;
	const v3s16 pos(1, 2, 3);
	UASSERT(queue.enqueue(pos, 0, 3));

	const u32 activated = queue.process(
			0, 0,
			[&](const ActiveObjectActivationQueue::Entry &entry) {
				UASSERT(queue.cancel(entry.blockpos));
				return ActiveObjectActivationQueue::Result::CONSUMED;
			},
			[]() { return (u64)0; });

	UASSERTCMP(u32, ==, activated, 1);
	UASSERT(queue.empty());
	UASSERTCMP(u64, ==, queue.pendingObjectCount(), 0);
}
