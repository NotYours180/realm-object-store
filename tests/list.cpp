#include "catch.hpp"

#include "util/test_file.hpp"
#include "util/index_helpers.hpp"

#include "binding_context.hpp"
#include "list.hpp"
#include "object_schema.hpp"
#include "property.hpp"
#include "results.hpp"
#include "schema.hpp"

#include "impl/realm_coordinator.hpp"

#include <realm/commit_log.hpp>
#include <realm/group_shared.hpp>
#include <realm/link_view.hpp>

using namespace realm;

TEST_CASE("list") {
    InMemoryTestFile config;
    config.automatic_change_notifications = false;
    config.cache = false;
    config.schema = std::make_unique<Schema>(Schema{
        {"origin", "", {
            {"array", PropertyTypeArray, "target"}
        }},
        {"target", "", {
            {"value", PropertyTypeInt}
        }},
    });

    auto r = Realm::get_shared_realm(config);
    auto& coordinator = *_impl::RealmCoordinator::get_existing_coordinator(config.path);

    auto origin = r->read_group()->get_table("class_origin");
    auto target = r->read_group()->get_table("class_target");

    r->begin_transaction();

    target->add_empty_row(10);
    for (int i = 0; i < 10; ++i)
        target->set_int(0, i, i);

    origin->add_empty_row(2);
    LinkViewRef lv = origin->get_linklist(0, 0);
    for (int i = 0; i < 10; ++i)
        lv->add(i);
    LinkViewRef lv2 = origin->get_linklist(0, 1);
    for (int i = 0; i < 10; ++i)
        lv2->add(i);

    r->commit_transaction();

    SECTION("add_notification_block()") {
        CollectionChangeIndices change;
        List lst(r, *r->config().schema->find("origin"), lv);

        auto write = [&](auto&& f) {
            r->begin_transaction();
            f();
            r->commit_transaction();

            advance_and_notify(*r);
        };

        auto require_change = [&] {
            return lst.add_notification_callback([&](CollectionChangeIndices c, std::exception_ptr err) {
                change = c;
            });
        };

        auto require_no_change = [&] {
            return lst.add_notification_callback([&](CollectionChangeIndices c, std::exception_ptr err) {
                REQUIRE(false);
            });
        };

        SECTION("modifying the list sends a change notifications") {
            auto token = require_change();
            write([&] { lst.remove(5); });
            REQUIRE_INDICES(change.deletions, 5);
        }

        SECTION("modifying a different list doesn't send a change notification") {
            auto token = require_no_change();
            write([&] { lv2->remove(5); });
        }

        SECTION("deleting the list sends a change notification") {
            auto token = require_change();
            write([&] { origin->move_last_over(0); });
            REQUIRE_INDICES(change.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
        }

        SECTION("modifying one of the target rows sends a change notification") {
            auto token = require_change();
            write([&] { lst.get(5).set_int(0, 6); });
            REQUIRE_INDICES(change.modifications, 5);
        }

        SECTION("deleting a target row sends a change notification") {
            auto token = require_change();
            write([&] { target->move_last_over(5); });
            REQUIRE_INDICES(change.deletions, 5);
        }

        SECTION("adding a row and then modifying the target row does not mark the row as modified") {
            auto token = require_change();
            write([&] {
                lst.add(5);
                target->set_int(0, 5, 10);
            });
            REQUIRE_INDICES(change.insertions, 10);
            REQUIRE_INDICES(change.modifications, 5);
        }

        SECTION("modifying and then moving a row reports move/insert but not modification") {
            auto token = require_change();
            write([&] {
                target->set_int(0, 5, 10);
                lst.move(5, 8);
            });
            REQUIRE_INDICES(change.insertions, 8);
            REQUIRE_INDICES(change.deletions, 5);
            REQUIRE_MOVES(change, {5, 8});
            REQUIRE(change.modifications.empty());
        }

        SECTION("modifying a row which appears multiple times in a list marks them all as modified") {
            r->begin_transaction();
            lst.add(5);
            r->commit_transaction();

            auto token = require_change();
            write([&] { target->set_int(0, 5, 10); });
            REQUIRE_INDICES(change.modifications, 5, 10);
        }

        SECTION("deleting a row which appears multiple times in a list marks them all as modified") {
            r->begin_transaction();
            lst.add(5);
            r->commit_transaction();

            auto token = require_change();
            write([&] { target->move_last_over(5); });
            REQUIRE_INDICES(change.deletions, 5, 10);
        }

        SECTION("clearing the target table sends a change notification") {
            auto token = require_change();
            write([&] { target->clear(); });
            REQUIRE_INDICES(change.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
        }

        SECTION("moving a target row does not send a change notification") {
            // Remove a row from the LV so that we have one to delete that's not in the list
            r->begin_transaction();
            lv->remove(2);
            r->commit_transaction();

            auto token = require_no_change();
            write([&] { target->move_last_over(2); });
        }

        SECTION("multiple LinkViws for the same LinkList can get notifications") {
            r->begin_transaction();
            target->clear();
            target->add_empty_row(5);
            r->commit_transaction();

            auto get_list = [&] {
                auto r = Realm::get_shared_realm(config);
                auto lv = r->read_group()->get_table("class_origin")->get_linklist(0, 0);
                return List(r, *r->config().schema->find("origin"), lv);
            };
            auto change_list = [&] {
                r->begin_transaction();
                if (lv->size()) {
                    target->set_int(0, lv->size() - 1, lv->size());
                }
                lv->add(lv->size());
                r->commit_transaction();
            };

            List lists[3];
            NotificationToken tokens[3];
            CollectionChangeIndices changes[3];

            for (int i = 0; i < 3; ++i) {
                lists[i] = get_list();
                tokens[i] = lists[i].add_notification_callback([i, &changes](CollectionChangeIndices c, std::exception_ptr) {
                    changes[i] = std::move(c);
                });
                change_list();
            }

            // Each of the Lists now has a different source version and state at
            // that version, so they should all see different changes despite
            // being for the same LinkList
            advance_and_notify(*r);

            REQUIRE_INDICES(changes[0].insertions, 0, 1, 2);
            REQUIRE(changes[0].modifications.empty());

            REQUIRE_INDICES(changes[1].insertions, 1, 2);
            REQUIRE_INDICES(changes[1].modifications, 0);

            REQUIRE_INDICES(changes[2].insertions, 2);
            REQUIRE_INDICES(changes[2].modifications, 1);

            // After making another change, they should all get the same notification
            change_list();
            advance_and_notify(*r);

            for (int i = 0; i < 3; ++i) {
                REQUIRE_INDICES(changes[i].insertions, 3);
                REQUIRE_INDICES(changes[i].modifications, 2);
            }
        }
    }

    SECTION("sort()") {
        auto objectschema = &*r->config().schema->find("origin");
        List list(r, *objectschema, lv);
        auto results = list.sort({{0}, {false}});

        REQUIRE(&results.get_object_schema() == objectschema);
        REQUIRE(results.get_mode() == Results::Mode::LinkView);
        REQUIRE(results.size() == 10);
        REQUIRE(results.sum(0) == 45);

        for (size_t i = 0; i < 10; ++i) {
            REQUIRE(results.get(i).get_index() == 9 - i);
        }
    }

    SECTION("filter()") {
        auto objectschema = &*r->config().schema->find("origin");
        List list(r, *objectschema, lv);
        auto results = list.filter(target->where().greater(0, 5));

        REQUIRE(&results.get_object_schema() == objectschema);
        REQUIRE(results.get_mode() == Results::Mode::Query);
        REQUIRE(results.size() == 4);

        for (size_t i = 0; i < 4; ++i) {
            REQUIRE(results.get(i).get_index() == i + 6);
        }
    }
}
