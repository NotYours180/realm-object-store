////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"

#include "util/index_helpers.hpp"
#include "util/test_file.hpp"

#include "impl/collection_notifier.hpp"
#include "impl/transact_log_handler.hpp"
#include "binding_context.hpp"
#include "property.hpp"
#include "object_schema.hpp"
#include "schema.hpp"

#include <realm/group_shared.hpp>
#include <realm/link_view.hpp>

using namespace realm;

class CaptureHelper {
public:
    CaptureHelper(std::string const& path, SharedRealm const& r, LinkViewRef lv, size_t table_ndx)
    : m_history(make_history(path))
#ifdef REALM_GROUP_SHARED_OPTIONS_HPP
    , m_sg(*m_history, SharedGroupOptions(SharedGroupOptions::Durability::MemOnly))
#else
    , m_sg(*m_history, SharedGroup::durability_MemOnly)
#endif
    , m_realm(r)
    , m_group(m_sg.begin_read())
    , m_linkview(lv)
    , m_table_ndx(table_ndx)
    {
        m_realm->begin_transaction();

        m_initial.reserve(lv->size());
        for (size_t i = 0; i < lv->size(); ++i)
            m_initial.push_back(lv->get(i).get_int(0));
    }

    CollectionChangeSet finish() {
        m_realm->commit_transaction();

        _impl::CollectionChangeBuilder c;
        _impl::TransactionChangeInfo info;
        info.lists.push_back({m_table_ndx, 0, 0, &c});
        info.table_modifications_needed.resize(m_group.size(), true);
        info.table_moves_needed.resize(m_group.size(), true);
        _impl::transaction::advance(m_sg, info);

        if (info.lists.empty()) {
            REQUIRE(!m_linkview->is_attached());
            return {};
        }

        validate(c);
        return c;
    }

    explicit operator bool() const { return m_realm->is_in_transaction(); }

private:
    std::unique_ptr<Replication> m_history;
    SharedGroup m_sg;
    SharedRealm m_realm;
    Group const& m_group;

    LinkViewRef m_linkview;
    std::vector<int_fast64_t> m_initial;
    size_t m_table_ndx;

    void validate(CollectionChangeSet const& info)
    {
        info.insertions.verify();
        info.deletions.verify();
        info.modifications.verify();

        std::vector<size_t> move_sources;
        for (auto const& move : info.moves)
            move_sources.push_back(m_initial[move.from]);

        // Apply the changes from the transaction log to our copy of the
        // initial, using UITableView's batching rules (i.e. delete, then
        // insert, then update)
        auto it = util::make_reverse_iterator(info.deletions.end());
        auto end = util::make_reverse_iterator(info.deletions.begin());
        for (; it != end; ++it) {
            m_initial.erase(m_initial.begin() + it->first, m_initial.begin() + it->second);
        }

        for (auto const& range : info.insertions) {
            for (auto i = range.first; i < range.second; ++i)
                m_initial.insert(m_initial.begin() + i, m_linkview->get(i).get_int(0));
        }

        for (auto const& range : info.modifications) {
            for (auto i = range.first; i < range.second; ++i)
                m_initial[i] = m_linkview->get(i).get_int(0);
        }

        REQUIRE(m_linkview->is_attached());

        // and make sure we end up with the same end result
        REQUIRE(m_initial.size() == m_linkview->size());
        for (size_t i = 0; i < m_initial.size(); ++i)
            CHECK(m_initial[i] == m_linkview->get(i).get_int(0));

        // Verify that everything marked as a move actually is one
        for (size_t i = 0; i < move_sources.size(); ++i) {
            if (!info.modifications.contains(info.moves[i].to)) {
                CHECK(m_linkview->get(info.moves[i].to).get_int(0) == move_sources[i]);
            }
        }
    }
};

TEST_CASE("Transaction log parsing: schema change validation") {
    InMemoryTestFile config;
    config.automatic_change_notifications = false;

    SECTION("Automatic") {
        auto r = Realm::get_shared_realm(config);
        r->update_schema({
            {"table", {
                {"unindexed", PropertyType::Int},
                {"indexed", PropertyType::Int, "", "", false, true}
            }},
        });
        r->read_group();

        auto history = make_history(config.path);

#ifdef REALM_GROUP_SHARED_OPTIONS_HPP
        SharedGroup sg(*history, SharedGroupOptions(SharedGroupOptions::Durability::MemOnly));
#else
        SharedGroup sg(*history, SharedGroup::durability_MemOnly);
#endif

        SECTION("adding a table is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.add_table("new table");
            table->add_column(type_String, "new col");
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("adding an index to an existing column is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->add_search_index(0);
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("removing an index from an existing column is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->remove_search_index(1);
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("adding a column to an existing table is not allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->add_column(type_String, "new col");
            wt.commit();

            REQUIRE_THROWS(r->refresh());
        }

        SECTION("removing a column is not allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->remove_column(1);
            wt.commit();

            REQUIRE_THROWS(r->refresh());
        }

        SECTION("removing a table is not allowed") {
            WriteTransaction wt(sg);
            wt.get_group().remove_table("class_table");
            wt.commit();

            REQUIRE_THROWS(r->refresh());
        }

        SECTION("the realm is left in a useable state after a rejected change") {
            r->begin_transaction();
            TableRef table = r->read_group().get_table("class_table");
            table->add_empty_row();
            r->commit_transaction();

            {
                WriteTransaction wt(sg);
                TableRef table = wt.get_table("class_table");
                table->insert_column(0, type_String, "new col");
                wt.commit();
            }

            REQUIRE_THROWS(r->refresh());
            REQUIRE(table->get_int(0, 0) == 0);
        }
    }

    SECTION("Additive") {
        config.schema_mode = SchemaMode::Additive;
        auto r = Realm::get_shared_realm(config);
        r->update_schema({
            {"table", {
                {"unindexed", PropertyType::Int},
                {"indexed", PropertyType::Int, "", "", false, true}
            }},
        });
        r->read_group();

        auto history = make_history(config.path);

#ifdef REALM_GROUP_SHARED_OPTIONS_HPP
        SharedGroup sg(*history, SharedGroupOptions(SharedGroupOptions::Durability::MemOnly));
#else
        SharedGroup sg(*history, SharedGroup::durability_MemOnly);
#endif

        SECTION("adding a table is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.add_table("new table");
            table->add_column(type_String, "new col");
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("adding an index to an existing column is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->add_search_index(0);
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("removing an index from an existing column is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->remove_search_index(1);
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("adding a column at the end of an existing table is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->add_column(type_String, "new col");
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("adding a column at the beginning of an existing table is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->insert_column(0, type_String, "new col");
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("moving columns is allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            _impl::TableFriend::move_column(*table->get_descriptor(), 0, 1);
            wt.commit();

            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("moving tables is allowed") {
            WriteTransaction wt(sg);
            wt.get_group().move_table(2, 0);
            wt.commit();
            REQUIRE_NOTHROW(r->refresh());
        }

        SECTION("removing a column is not allowed") {
            WriteTransaction wt(sg);
            TableRef table = wt.get_table("class_table");
            table->remove_column(1);
            wt.commit();

            REQUIRE_THROWS(r->refresh());
        }

        SECTION("removing a table is not allowed") {
            WriteTransaction wt(sg);
            wt.get_group().remove_table("class_table");
            wt.commit();

            REQUIRE_THROWS(r->refresh());
        }
    }
}

TEST_CASE("Transaction log parsing: changeset calcuation") {
    InMemoryTestFile config;
    config.automatic_change_notifications = false;

    SECTION("table change information") {
        auto r = Realm::get_shared_realm(config);
        r->update_schema({
            {"table", {
                {"pk", PropertyType::Int, "", "", true, true},
                {"value", PropertyType::Int}
            }},
        });

        auto& table = *r->read_group().get_table("class_table");

        r->begin_transaction();
        table.add_empty_row(10);
        for (int i = 9; i >= 0; --i) {
            table.set_int_unique(0, i, i);
            table.set_int(1, i, i);
        }
        r->commit_transaction();

        auto track_changes = [&](std::vector<bool> tables_needed, auto&& f) {
            auto history = make_history(config.path);
#ifdef REALM_GROUP_SHARED_OPTIONS_HPP
            SharedGroup sg(*history, SharedGroupOptions(SharedGroupOptions::Durability::MemOnly));
#else
            SharedGroup sg(*history, SharedGroup::durability_MemOnly);
#endif
            sg.begin_read();

            r->begin_transaction();
            f();
            r->commit_transaction();

            _impl::TransactionChangeInfo info;
            info.table_modifications_needed = tables_needed;
            info.table_moves_needed = tables_needed;
            _impl::transaction::advance(sg, info);
            return info;
        };

        SECTION("modifying a row marks it as modified") {
            auto info = track_changes({false, false, true}, [&] {
                table.set_int(0, 1, 2);
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[2].modifications, 1);
        }

        SECTION("modifications to untracked tables are ignored") {
            auto info = track_changes({false, false, false}, [&] {
                table.set_int(0, 1, 2);
            });
            REQUIRE(info.tables.empty());
        }

        SECTION("new row additions are reported") {
            auto info = track_changes({false, false, true}, [&] {
                table.add_empty_row();
                table.add_empty_row();
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[2].insertions, 10, 11);
        }

        SECTION("deleting newly added rows makes them not be reported") {
            auto info = track_changes({false, false, true}, [&] {
                table.add_empty_row();
                table.add_empty_row();
                table.move_last_over(11);
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[2].insertions, 10);
            REQUIRE(info.tables[2].deletions.empty());
        }

        SECTION("modifying newly added rows is reported as a modification") {
            auto info = track_changes({false, false, true}, [&] {
                table.add_empty_row();
                table.set_int(0, 10, 10);
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[2].insertions, 10);
            REQUIRE_INDICES(info.tables[2].modifications, 10);
        }

        SECTION("move_last_over() does not shift rows other than the last one") {
            auto info = track_changes({false, false, true}, [&] {
                table.move_last_over(2);
                table.move_last_over(3);
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[2].deletions, 2, 3, 8, 9);
            REQUIRE_INDICES(info.tables[2].insertions, 2, 3);
            REQUIRE_MOVES(info.tables[2], {8, 3}, {9, 2});
        }

        SECTION("inserting new tables does not distrupt change tracking") {
            auto info = track_changes({false, false, true}, [&] {
                table.add_empty_row();
                r->read_group().insert_table(0, "new table");
                table.add_empty_row();
            });
            REQUIRE(info.tables.size() == 4);
            REQUIRE_INDICES(info.tables[3].insertions, 10, 11);
        }

        SECTION("reordering tables does not distrupt change tracking") {
            auto info = track_changes({false, false, true}, [&] {
                table.add_empty_row();
                r->read_group().move_table(2, 0);
                table.add_empty_row();
                r->read_group().move_table(0, 1);
                table.add_empty_row();
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[1].insertions, 10, 11, 12);
        }

        SECTION("swap_rows() reports a pair of moves") {
            auto info = track_changes({false, false, true}, [&] {
                table.swap_rows(1, 5);
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[2].deletions, 1, 5);
            REQUIRE_INDICES(info.tables[2].insertions, 1, 5);
            REQUIRE_MOVES(info.tables[2], {1, 5}, {5, 1});
        }

        SECTION("swap_rows() preserves modifications from before the swap") {
            auto info = track_changes({false, false, true}, [&] {
                table.set_int(1, 8, 15);
                table.swap_rows(8, 9);
                table.move_last_over(8);
            });
            REQUIRE(info.tables.size() == 3);
            auto& table = info.tables[2];
            REQUIRE(table.insertions.empty());
            REQUIRE(table.moves.empty());
            REQUIRE_INDICES(table.deletions, 9);
            REQUIRE_INDICES(table.modifications, 8);
        }

#if REALM_VER_MAJOR >= 2
        SECTION("PK conflict from last row produces no net change") {
            auto info = track_changes({false, false, true}, [&] {
                table.add_empty_row();
                table.set_int_unique(0, 10, 5);
            });
            REQUIRE(info.tables.size() == 3);
            // new row is inserted at 10, then moved over 5 and assumes the
            // identity of the one which was at 5, so nothing actually happened
            REQUIRE(info.tables[2].empty());
        }

        SECTION("moving a row via a PK conflict marks it as moved") {
            auto info = track_changes({false, false, true}, [&] {
                table.add_empty_row(2);
                table.set_int_unique(0, 10, 5);
            });
            REQUIRE(info.tables.size() == 3);
            // 10 assumed identity of old 5, but 11 was moved over it, so 5
            // is a new insert and 10 is a move.
            REQUIRE_INDICES(info.tables[2].insertions, 5, 10);
            REQUIRE_INDICES(info.tables[2].deletions, 5);
            REQUIRE_MOVES(info.tables[2], {5, 10});
        }

        SECTION("modifying a row before a PK-conflict move marks it as modified") {
            auto info = track_changes({false, false, true}, [&] {
                table.set_int(1, 5, 15);
                table.add_empty_row(2);
                table.set_int_unique(0, 10, 5);
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[2].modifications, 10);
        }

        SECTION("modifying a row after a PK-conflict move marks it as modified") {
            auto info = track_changes({false, false, true}, [&] {
                table.add_empty_row(2);
                table.set_int_unique(0, 10, 5);
                table.set_int(1, 10, 15);
            });
            REQUIRE(info.tables.size() == 3);
            REQUIRE_INDICES(info.tables[2].modifications, 10);
        }

        SECTION("non-conflicting set_int_unique() does not mark a row as modified") {
            auto info = track_changes({false, false, true}, [&] {
                table.set_int_unique(0, 0, 20);
            });
            REQUIRE(info.tables.empty());
        }

        SECTION("SetDefault does not mark a row as modified") {
            auto info = track_changes({false, false, true}, [&] {
                bool is_default = true;
                table.set_int(0, 0, 1, is_default);
            });
            REQUIRE(info.tables.empty());
        }
#endif
    }

    SECTION("LinkView change information") {
        auto r = Realm::get_shared_realm(config);
        r->update_schema({
            {"origin", {
                {"array", PropertyType::Array, "target"}
            }},
            {"target", {
                {"value", PropertyType::Int}
            }},
        });

        auto origin = r->read_group().get_table("class_origin");
        auto target = r->read_group().get_table("class_target");

        r->begin_transaction();

        target->add_empty_row(10);
        for (int i = 0; i < 10; ++i)
            target->set_int(0, i, i);

        origin->add_empty_row();
        LinkViewRef lv = origin->get_linklist(0, 0);
        for (int i = 0; i < 10; ++i)
            lv->add(i);

        r->commit_transaction();

#define VALIDATE_CHANGES(out) \
    for (CaptureHelper helper(config.path, r, lv, origin->get_index_in_group()); helper; out = helper.finish())

        CollectionChangeSet changes;
        SECTION("single change type") {
            SECTION("add single") {
                VALIDATE_CHANGES(changes) {
                    lv->add(0);
                }
                REQUIRE_INDICES(changes.insertions, 10);
            }
            SECTION("add multiple") {
                VALIDATE_CHANGES(changes) {
                    lv->add(0);
                    lv->add(0);
                }
                REQUIRE_INDICES(changes.insertions, 10, 11);
            }

            SECTION("erase single") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(5);
                }
                REQUIRE_INDICES(changes.deletions, 5);
            }
            SECTION("erase contiguous forward") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(5);
                    lv->remove(5);
                    lv->remove(5);
                }
                REQUIRE_INDICES(changes.deletions, 5, 6, 7);
            }
            SECTION("erase contiguous reverse") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(7);
                    lv->remove(6);
                    lv->remove(5);
                }
                REQUIRE_INDICES(changes.deletions, 5, 6, 7);
            }
            SECTION("erase contiguous mixed") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(5);
                    lv->remove(6);
                    lv->remove(5);
                }
                REQUIRE_INDICES(changes.deletions, 5, 6, 7);
            }
            SECTION("erase scattered forward") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(3);
                    lv->remove(4);
                    lv->remove(5);
                }
                REQUIRE_INDICES(changes.deletions, 3, 5, 7);
            }
            SECTION("erase scattered backwards") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(7);
                    lv->remove(5);
                    lv->remove(3);
                }
                REQUIRE_INDICES(changes.deletions, 3, 5, 7);
            }
            SECTION("erase scattered mixed") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(3);
                    lv->remove(6);
                    lv->remove(4);
                }
                REQUIRE_INDICES(changes.deletions, 3, 5, 7);
            }

            SECTION("set single") {
                VALIDATE_CHANGES(changes) {
                    lv->set(5, 0);
                }
                REQUIRE_INDICES(changes.modifications, 5);
            }
            SECTION("set contiguous") {
                VALIDATE_CHANGES(changes) {
                    lv->set(5, 0);
                    lv->set(6, 0);
                    lv->set(7, 0);
                }
                REQUIRE_INDICES(changes.modifications, 5, 6, 7);
            }
            SECTION("set scattered") {
                VALIDATE_CHANGES(changes) {
                    lv->set(5, 0);
                    lv->set(7, 0);
                    lv->set(9, 0);
                }
                REQUIRE_INDICES(changes.modifications, 5, 7, 9);
            }
            SECTION("set redundant") {
                VALIDATE_CHANGES(changes) {
                    lv->set(5, 0);
                    lv->set(5, 0);
                    lv->set(5, 0);
                }
                REQUIRE_INDICES(changes.modifications, 5);
            }

            SECTION("clear") {
                VALIDATE_CHANGES(changes) {
                    lv->clear();
                }
                REQUIRE_INDICES(changes.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
            }

            SECTION("move backward") {
                VALIDATE_CHANGES(changes) {
                    lv->move(5, 3);
                }
                REQUIRE_MOVES(changes, {5, 3});
            }

            SECTION("move forward") {
                VALIDATE_CHANGES(changes) {
                    lv->move(1, 3);
                }
                REQUIRE_MOVES(changes, {1, 3});
            }

            SECTION("chained moves") {
                VALIDATE_CHANGES(changes) {
                    lv->move(1, 3);
                    lv->move(3, 5);
                }
                REQUIRE_MOVES(changes, {1, 5});
            }

            SECTION("backwards chained moves") {
                VALIDATE_CHANGES(changes) {
                    lv->move(5, 3);
                    lv->move(3, 1);
                }
                REQUIRE_MOVES(changes, {5, 1});
            }

            SECTION("moves shifting other moves") {
                VALIDATE_CHANGES(changes) {
                    lv->move(1, 5);
                    lv->move(2, 7);
                }
                REQUIRE_MOVES(changes, {1, 4}, {3, 7});

                VALIDATE_CHANGES(changes) {
                    lv->move(1, 5);
                    lv->move(7, 0);
                }
                REQUIRE_MOVES(changes, {1, 6}, {7, 0});
            }

            SECTION("move to current location is a no-op") {
                VALIDATE_CHANGES(changes) {
                    lv->move(5, 5);
                }
                REQUIRE(changes.insertions.empty());
                REQUIRE(changes.deletions.empty());
                REQUIRE(changes.moves.empty());
            }

            SECTION("delete a target row") {
                VALIDATE_CHANGES(changes) {
                    target->move_last_over(5);
                }
                REQUIRE_INDICES(changes.deletions, 5);
            }

            SECTION("delete all target rows") {
                VALIDATE_CHANGES(changes) {
                    lv->remove_all_target_rows();
                }
                REQUIRE_INDICES(changes.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
            }

            SECTION("clear target table") {
                VALIDATE_CHANGES(changes) {
                    target->clear();
                }
                REQUIRE_INDICES(changes.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
            }

            SECTION("swap()") {
                VALIDATE_CHANGES(changes) {
                    lv->swap(3, 5);
                }
                REQUIRE_INDICES(changes.modifications, 3, 5);
            }
        }

        SECTION("mixed change types") {
            SECTION("set -> insert") {
                VALIDATE_CHANGES(changes) {
                    lv->set(5, 0);
                    lv->insert(5, 0);
                }
                REQUIRE_INDICES(changes.insertions, 5);
                REQUIRE_INDICES(changes.modifications, 6);

                VALIDATE_CHANGES(changes) {
                    lv->set(4, 0);
                    lv->insert(5, 0);
                }
                REQUIRE_INDICES(changes.insertions, 5);
                REQUIRE_INDICES(changes.modifications, 4);
            }
            SECTION("insert -> set") {
                VALIDATE_CHANGES(changes) {
                    lv->insert(5, 0);
                    lv->set(5, 1);
                }
                REQUIRE_INDICES(changes.insertions, 5);
                REQUIRE_INDICES(changes.modifications, 5);

                VALIDATE_CHANGES(changes) {
                    lv->insert(5, 0);
                    lv->set(6, 1);
                }
                REQUIRE_INDICES(changes.insertions, 5);
                REQUIRE_INDICES(changes.modifications, 6);

                VALIDATE_CHANGES(changes) {
                    lv->insert(6, 0);
                    lv->set(5, 1);
                }
                REQUIRE_INDICES(changes.insertions, 6);
                REQUIRE_INDICES(changes.modifications, 5);
            }

            SECTION("set -> erase") {
                VALIDATE_CHANGES(changes) {
                    lv->set(5, 0);
                    lv->remove(5);
                }
                REQUIRE_INDICES(changes.deletions, 5);
                REQUIRE(changes.modifications.empty());

                VALIDATE_CHANGES(changes) {
                    lv->set(5, 0);
                    lv->remove(4);
                }
                REQUIRE_INDICES(changes.deletions, 4);
                REQUIRE_INDICES(changes.modifications, 4);

                VALIDATE_CHANGES(changes) {
                    lv->set(5, 0);
                    lv->remove(4);
                    lv->remove(4);
                }
                REQUIRE_INDICES(changes.deletions, 4, 5);
                REQUIRE(changes.modifications.empty());
            }

            SECTION("erase -> set") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(5);
                    lv->set(5, 0);
                }
                REQUIRE_INDICES(changes.deletions, 5);
                REQUIRE_INDICES(changes.modifications, 5);
            }

            SECTION("insert -> clear") {
                VALIDATE_CHANGES(changes) {
                    lv->add(0);
                    lv->clear();
                }
                REQUIRE_INDICES(changes.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
                REQUIRE(changes.insertions.empty());
            }

            SECTION("set -> clear") {
                VALIDATE_CHANGES(changes) {
                    lv->set(0, 5);
                    lv->clear();
                }
                REQUIRE_INDICES(changes.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
                REQUIRE(changes.modifications.empty());
            }

            SECTION("clear -> insert") {
                VALIDATE_CHANGES(changes) {
                    lv->clear();
                    lv->add(0);
                }
                REQUIRE_INDICES(changes.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
                REQUIRE_INDICES(changes.insertions, 0);
            }

            SECTION("insert -> delete") {
                VALIDATE_CHANGES(changes) {
                    lv->add(0);
                    lv->remove(10);
                }
                REQUIRE(changes.insertions.empty());
                REQUIRE(changes.deletions.empty());

                VALIDATE_CHANGES(changes) {
                    lv->add(0);
                    lv->remove(9);
                }
                REQUIRE_INDICES(changes.deletions, 9);
                REQUIRE_INDICES(changes.insertions, 9);

                VALIDATE_CHANGES(changes) {
                    lv->insert(1, 1);
                    lv->insert(3, 3);
                    lv->insert(5, 5);
                    lv->remove(6);
                    lv->remove(4);
                    lv->remove(2);
                }
                REQUIRE_INDICES(changes.deletions, 1, 2, 3);
                REQUIRE_INDICES(changes.insertions, 1, 2, 3);

                VALIDATE_CHANGES(changes) {
                    lv->insert(1, 1);
                    lv->insert(3, 3);
                    lv->insert(5, 5);
                    lv->remove(2);
                    lv->remove(3);
                    lv->remove(4);
                }
                REQUIRE_INDICES(changes.deletions, 1, 2, 3);
                REQUIRE_INDICES(changes.insertions, 1, 2, 3);
            }

            SECTION("delete -> insert") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(9);
                    lv->add(0);
                }
                REQUIRE_INDICES(changes.deletions, 9);
                REQUIRE_INDICES(changes.insertions, 9);
            }

            SECTION("interleaved delete and insert") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(9);
                    lv->remove(7);
                    lv->remove(5);
                    lv->remove(3);
                    lv->remove(1);

                    lv->insert(4, 9);
                    lv->insert(3, 7);
                    lv->insert(2, 5);
                    lv->insert(1, 3);
                    lv->insert(0, 1);

                    lv->remove(9);
                    lv->remove(7);
                    lv->remove(5);
                    lv->remove(3);
                    lv->remove(1);
                }

                REQUIRE_INDICES(changes.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
                REQUIRE_INDICES(changes.insertions, 0, 1, 2, 3, 4);
            }

            SECTION("move after set is just insert+delete") {
                VALIDATE_CHANGES(changes) {
                    lv->set(5, 6);
                    lv->move(5, 0);
                }

                REQUIRE_INDICES(changes.deletions, 5);
                REQUIRE_INDICES(changes.insertions, 0);
                REQUIRE_MOVES(changes, {5, 0});
            }

            SECTION("set after move is just insert+delete") {
                VALIDATE_CHANGES(changes) {
                    lv->move(5, 0);
                    lv->set(0, 6);
                }

                REQUIRE_INDICES(changes.deletions, 5);
                REQUIRE_INDICES(changes.insertions, 0);
                REQUIRE_MOVES(changes, {5, 0});
            }

            SECTION("delete after move removes original row") {
                VALIDATE_CHANGES(changes) {
                    lv->move(5, 0);
                    lv->remove(0);
                }

                REQUIRE_INDICES(changes.deletions, 5);
                REQUIRE(changes.moves.empty());
            }

            SECTION("moving newly inserted row just changes reported index of insert") {
                VALIDATE_CHANGES(changes) {
                    lv->move(5, 0);
                    lv->remove(0);
                }

                REQUIRE_INDICES(changes.deletions, 5);
                REQUIRE(changes.moves.empty());
            }

            SECTION("moves shift insertions/changes like any other insertion") {
                VALIDATE_CHANGES(changes) {
                    lv->insert(5, 5);
                    lv->set(6, 6);
                    lv->move(7, 4);
                }
                REQUIRE_INDICES(changes.deletions, 6);
                REQUIRE_INDICES(changes.insertions, 4, 6);
                REQUIRE_INDICES(changes.modifications, 7);
                REQUIRE_MOVES(changes, {6, 4});
            }

            SECTION("clear after delete") {
                VALIDATE_CHANGES(changes) {
                    lv->remove(5);
                    lv->clear();
                }
                REQUIRE_INDICES(changes.deletions, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
            }

            SECTION("erase before previous move target") {
                VALIDATE_CHANGES(changes) {
                    lv->move(2, 8);
                    lv->remove(5);
                }
                REQUIRE_INDICES(changes.insertions, 7);
                REQUIRE_INDICES(changes.deletions, 2, 6);
                REQUIRE_MOVES(changes, {2, 7});
            }

            SECTION("insert after move updates move destination") {
                VALIDATE_CHANGES(changes) {
                    lv->move(2, 8);
                    lv->insert(5, 5);
                }
                REQUIRE_MOVES(changes, {2, 9});
            }
        }

        SECTION("deleting the linkview") {
            SECTION("directly") {
                VALIDATE_CHANGES(changes) {
                    origin->move_last_over(0);
                }
                REQUIRE(!lv->is_attached());
                REQUIRE(changes.insertions.empty());
                REQUIRE(changes.deletions.empty());
                REQUIRE(changes.modifications.empty());
            }

            SECTION("table clear") {
                VALIDATE_CHANGES(changes) {
                    origin->clear();
                }
                REQUIRE(!lv->is_attached());
                REQUIRE(changes.insertions.empty());
                REQUIRE(changes.deletions.empty());
                REQUIRE(changes.modifications.empty());
            }

            SECTION("delete a different lv") {
                r->begin_transaction();
                origin->add_empty_row();
                r->commit_transaction();

                VALIDATE_CHANGES(changes) {
                    origin->move_last_over(1);
                }
                REQUIRE(changes.insertions.empty());
                REQUIRE(changes.deletions.empty());
                REQUIRE(changes.modifications.empty());
            }
        }

        SECTION("modifying a different linkview should not produce notifications") {
            r->begin_transaction();
            origin->add_empty_row();
            LinkViewRef lv2 = origin->get_linklist(0, 1);
            lv2->add(5);
            r->commit_transaction();

            VALIDATE_CHANGES(changes) {
                lv2->add(1);
                lv2->add(2);
                lv2->remove(0);
                lv2->set(0, 6);
                lv2->move(1, 0);
                lv2->swap(0, 1);
                lv2->clear();
                lv2->add(1);
            }

            REQUIRE(changes.insertions.empty());
            REQUIRE(changes.deletions.empty());
            REQUIRE(changes.modifications.empty());
        }

        SECTION("inserting new tables does not distrupt change tracking") {
            VALIDATE_CHANGES(changes) {
                lv->add(0);
                r->read_group().insert_table(0, "new table");
                lv->add(0);
            }
            REQUIRE_INDICES(changes.insertions, 10, 11);
        }

        SECTION("reordering tables does not distrupt change tracking") {
            VALIDATE_CHANGES(changes) {
                lv->add(0);
                r->read_group().move_table(2, 0);
                lv->add(0);
                r->read_group().move_table(0, 3);
                lv->add(0);
            }
            REQUIRE_INDICES(changes.insertions, 10, 11, 12);
        }

        SECTION("inserting new columns does not distrupt change tracking") {
            VALIDATE_CHANGES(changes) {
                lv->add(0);
                origin->insert_column(0, type_Int, "new column");
                lv->add(0);
            }
            REQUIRE_INDICES(changes.insertions, 10, 11);
        }

        SECTION("reordering columns does not distrupt change tracking") {
            VALIDATE_CHANGES(changes) {
                origin->insert_column(1, type_Int, "new column 1");
                origin->insert_column(2, type_Int, "new column 2");
                origin->insert_column(3, type_Int, "new column 3");

                lv->add(0);
                _impl::TableFriend::move_column(*origin->get_descriptor(), 0, 3);
                lv->add(0);
                _impl::TableFriend::move_column(*origin->get_descriptor(), 3, 1);
                lv->add(0);
            }
            REQUIRE_INDICES(changes.insertions, 10, 11, 12);
        }
    }

    SECTION("object change information") {
        config.cache = false;
        auto realm = Realm::get_shared_realm(config);
        realm->update_schema({
            {"origin", {
                {"link", PropertyType::Object, "target", "", false, false, true},
                {"array", PropertyType::Array, "target"}
            }},
            {"origin 2", {
                {"link", PropertyType::Object, "target", "", false, false, true},
                {"array", PropertyType::Array, "target"}
            }},
            {"target", {
                {"pk", PropertyType::Int, "", "", true, true},
                {"value 1", PropertyType::Int},
                {"value 2", PropertyType::Int},
            }},
        });

        auto origin = realm->read_group().get_table("class_origin");
        auto target = realm->read_group().get_table("class_target");

        realm->begin_transaction();

        target->add_empty_row(10);
        for (int i = 0; i < 10; ++i) {
            if (i > 0)
                target->set_int_unique(0, i, i);
            target->set_int(1, i, i);
            target->set_int(2, i, i);
        }

        origin->add_empty_row(2);
        origin->set_link(0, 0, 5);
        origin->set_link(0, 1, 6);
        LinkViewRef lv = origin->get_linklist(1, 0);
        for (int i = 0; i < 10; ++i)
            lv->add(i);
        LinkViewRef lv2 = origin->get_linklist(1, 1);
        lv2->add(0);

        realm->read_group().get_table("class_origin 2")->add_empty_row();

        realm->commit_transaction();

        class Context : public BindingContext {
        public:
            Context(std::initializer_list<Row> rows)
            {
                m_result.reserve(rows.size());
                for (auto& row : rows) {
                    m_result.push_back(ObserverState{row.get_table()->get_index_in_group(), row.get_index(),
                        (void *)(uintptr_t)m_result.size()});
                }
            }

            bool modified(size_t index, size_t col)
            {
                auto it = std::find_if(begin(m_result), end(m_result),
                                       [=](auto&& change) { return (void *)(uintptr_t)index == change.info; });
                if (it == m_result.end() || col >= it->changes.size())
                    return false;
                return it->changes[col].kind != BindingContext::ColumnInfo::Kind::None;
            }

            bool invalidated(size_t index)
            {
                return std::find(begin(m_invalidated), end(m_invalidated), (void *)(uintptr_t)index) != end(m_invalidated);
            }

            bool has_array_change(size_t index, size_t col, ColumnInfo::Kind kind, IndexSet values)
            {
                auto& changes = m_result[index].changes;
                if (changes.size() <= col)
                    return kind == ColumnInfo::Kind::None;
                auto& column = changes[col];
                return column.kind == kind && std::equal(column.indices.as_indexes().begin(), column.indices.as_indexes().end(),
                                                         values.as_indexes().begin(), values.as_indexes().end());
            }

        private:
            std::vector<ObserverState> m_result;
            std::vector<void*> m_invalidated;

            std::vector<ObserverState> get_observed_rows() override
            {
                return m_result;
            }

            void did_change(std::vector<ObserverState> const& observers,
                            std::vector<void*> const& invalidated) override
            {
                m_invalidated = invalidated;
                m_result = observers;
            }
        };

        auto observe = [&](std::initializer_list<Row> rows, auto&& fn) {
            auto history = make_history(config.path);
            SharedGroup sg(*history, SharedGroupOptions(SharedGroupOptions::Durability::MemOnly));
            auto& group = sg.begin_read();

            Context observer(rows);

            realm->begin_transaction();
            fn();
            realm->commit_transaction();

            _impl::transaction::advance(sg, &observer, SchemaMode::Automatic);
            return observer;
        };

        SECTION("setting a property marks that property as changed") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                r.set_int(0, 1);
            });
            REQUIRE(changes.modified(0, 0));
            REQUIRE_FALSE(changes.modified(0, 1));
            REQUIRE_FALSE(changes.modified(0, 2));
        }

        SECTION("self-assignment marks as changed") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                r.set_int(0, r.get_int(0));
            });
            REQUIRE(changes.modified(0, 0));
            REQUIRE_FALSE(changes.modified(0, 1));
            REQUIRE_FALSE(changes.modified(0, 2));
        }

        SECTION("SetDefault does not mark as changed") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                r.get_table()->set_int(0, r.get_index(), 5, true);
            });
            REQUIRE_FALSE(changes.modified(0, 0));
            REQUIRE_FALSE(changes.modified(0, 1));
            REQUIRE_FALSE(changes.modified(0, 2));

        }

        SECTION("multiple properties on a single object are handled properly") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                r.set_int(1, 1);
            });
            REQUIRE_FALSE(changes.modified(0, 0));
            REQUIRE(changes.modified(0, 1));
            REQUIRE_FALSE(changes.modified(0, 2));

            changes = observe({r}, [&] {
                r.set_int(2, 1);
            });
            REQUIRE_FALSE(changes.modified(0, 0));
            REQUIRE_FALSE(changes.modified(0, 1));
            REQUIRE(changes.modified(0, 2));

            changes = observe({r}, [&] {
                r.set_int(0, 1);
                r.set_int(2, 1);
            });
            REQUIRE(changes.modified(0, 0));
            REQUIRE_FALSE(changes.modified(0, 1));
            REQUIRE(changes.modified(0, 2));

            changes = observe({r}, [&] {
                r.set_int(0, 1);
                r.set_int(1, 1);
                r.set_int(2, 1);
            });
            REQUIRE(changes.modified(0, 0));
            REQUIRE(changes.modified(0, 1));
            REQUIRE(changes.modified(0, 2));
        }

        SECTION("setting other objects does not mark as changed") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                target->set_int(0, r.get_index() + 1, 5);
            });
            REQUIRE_FALSE(changes.modified(0, 0));
            REQUIRE_FALSE(changes.modified(0, 1));
            REQUIRE_FALSE(changes.modified(0, 2));
        }

        SECTION("deleting an observed object adds it to invalidated") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                r.move_last_over();
            });
            REQUIRE(changes.invalidated(0));
        }

        SECTION("deleting an unobserved object does nothing") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                target->move_last_over(r.get_index() + 1);
            });
            REQUIRE_FALSE(changes.invalidated(0));
        }

        SECTION("deleting the target of a link marks the link as modified") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                target->move_last_over(r.get_link(0));
            });
            REQUIRE(changes.modified(0, 0));
        }

        SECTION("clearing the target table of a link marks the link as modified") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                target->clear();
            });
            REQUIRE(changes.modified(0, 0));
        }

        SECTION("moving the target of a link does not mark the link as modified") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                target->swap_rows(5, 9);
            });
            REQUIRE_FALSE(changes.modified(0, 0));

            changes = observe({r}, [&] {
                target->move_last_over(0);
            });
            REQUIRE_FALSE(changes.modified(0, 0));
        }

        SECTION("clearing a table invalidates all observers for that table") {
            Row r1 = target->get(0);
            Row r2 = target->get(5);
            Row r3 = origin->get(0);
            auto changes = observe({r1, r2, r3}, [&] {
                target->clear();
            });
            REQUIRE(changes.invalidated(0));
            REQUIRE(changes.invalidated(1));
            REQUIRE_FALSE(changes.invalidated(2));
        }

        SECTION("moving an observed object with insert_empty_row() does not interfere with tracking") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                target->insert_empty_row(0);
                r.set_int(0, 5);
            });
            REQUIRE(changes.modified(0, 0));
        }

        SECTION("moving an observed object with move_last_over() does not interfere with tracking") {
            Row r = target->back();
            auto changes = observe({r}, [&] {
                target->move_last_over(0);
                r.set_int(0, 5);
            });
            REQUIRE(changes.modified(0, 0));
        }

        SECTION("moving an observed object with swap() does not interfere with tracking") {
            Row r1 = target->get(1), r2 = target->get(3);
            auto changes = observe({r1, r2}, [&] {
                target->swap_rows(r1.get_index(), r2.get_index());
                r1.set_int(0, 5);
                r2.set_int(1, 5);
            });

            REQUIRE(changes.modified(0, 0));
            REQUIRE_FALSE(changes.modified(0, 1));
            REQUIRE_FALSE(changes.modified(0, 2));

            REQUIRE_FALSE(changes.modified(1, 0));
            REQUIRE(changes.modified(1, 1));
            REQUIRE_FALSE(changes.modified(1, 2));
        }

#if 0 // not yet implemented
        SECTION("subsuming an observed object updates tracking to the new object") {

        }
#endif

        SECTION("inserting a column into an observed table does not break tracking") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                r.set_int(0, 5);
                target->insert_column(0, type_String, "col");
                r.set_int(3, 5);
            });
            REQUIRE_FALSE(changes.modified(0, 0));
            REQUIRE(changes.modified(0, 1));
            REQUIRE_FALSE(changes.modified(0, 2));
            REQUIRE(changes.modified(0, 3));
        }

        SECTION("moving columns in observed tables does not break tracking") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                r.set_int(0, 5);
                _impl::TableFriend::move_column(*target->get_descriptor(), 0, 1);
                r.set_int(2, 5);
            });
            REQUIRE_FALSE(changes.modified(0, 0));
            REQUIRE(changes.modified(0, 1));
            REQUIRE(changes.modified(0, 2));
        }

        SECTION("moving an observed table does not break tracking") {
            Row r = target->get(0);
            auto changes = observe({r}, [&] {
                r.set_int(0, 5);
                realm->read_group().move_table(r.get_table()->get_index_in_group(), 0);
                r.set_int(1, 5);
            });
            REQUIRE(changes.modified(0, 0));
            REQUIRE(changes.modified(0, 1));

            changes = observe({r}, [&] {
                r.set_int(0, 5);
                realm->read_group().insert_table(0, "new table");
                r.set_int(1, 5);
            });
            REQUIRE(changes.modified(0, 0));
            REQUIRE(changes.modified(0, 1));
        }

        using Kind = BindingContext::ColumnInfo::Kind;
        SECTION("array: add()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->add(0);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Insert, {10}));
        }

        SECTION("array: insert()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->insert(4, 0);
                lv->insert(2, 0);
                lv->insert(8, 0);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Insert, {2, 5, 8}));
        }

        SECTION("array: remove()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->remove(0);
                lv->remove(2);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Remove, {0, 3}));
        }

        SECTION("array: set()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->set(0, 3);
                lv->set(2, 3);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Set, {0, 2}));
        }

        SECTION("array: move()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->move(5, 3);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Set, {3, 4, 5}));
        }

        SECTION("array: swap()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->swap(5, 3);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Set, {3, 5}));
        }

        SECTION("array: clear()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->clear();
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Remove, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        }

        SECTION("array: clear() after add()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->add(0);
                lv->clear();
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Remove, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        }

        SECTION("array: clear() after set()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->set(5, 3);
                lv->clear();
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Remove, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        }

        SECTION("array: clear() after remove()") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->remove(2);
                lv->clear();
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Remove, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        }

        SECTION("array: multiple change kinds") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->add(0);
                lv->remove(0);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::SetAll, {}));
        }

        SECTION("array: modifying different array does not produce changes") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv2->add(0);
            });
            REQUIRE_FALSE(changes.modified(0, 1));
        }

        SECTION("array: modifying different table does not produce changes") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                realm->read_group().get_table("class_origin 2")->get_linklist(1, 0)->add(0);
            });
            REQUIRE_FALSE(changes.modified(0, 1));
        }

        SECTION("array: moving the observed object via insert_empty_row() does not interrupt tracking") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->add(0);
                origin->insert_empty_row(0);
                lv->add(0);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Insert, {10, 11}));
        }

#if 0 // not yet implemented
        SECTION("array: moving the observed object via swap() does not interrupt tracking") {
            Row r = origin->get(0);
            auto changes = observe({r}, [&] {
                lv->add(0);
                origin->swap_rows(0, 1);
                lv->add(0);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Insert, {10, 11}));
        }
#endif

        SECTION("array: moving the observed object via move_last_over() does not interrupt tracking") {
            Row r = origin->get(0);

            realm->begin_transaction();
            origin->swap_rows(0, 1);
            realm->commit_transaction();

            auto changes = observe({r}, [&] {
                lv->add(0);
                origin->move_last_over(0);
                lv->add(0);
            });
            REQUIRE(changes.has_array_change(0, 1, Kind::Insert, {10, 11}));
        }

        SECTION("array: moving the observed object via primary key subsumption does not interrupt tracking") {
        }
    }
}

TEST_CASE("DeepChangeChecker") {
    InMemoryTestFile config;
    config.automatic_change_notifications = false;
    auto r = Realm::get_shared_realm(config);
    r->update_schema({
        {"table", {
            {"int", PropertyType::Int},
            {"link", PropertyType::Object, "table", "", false, false, true},
            {"array", PropertyType::Array, "table"}
        }},
    });
    auto table = r->read_group().get_table("class_table");

    r->begin_transaction();
    table->add_empty_row(10);
    for (int i = 0; i < 10; ++i)
        table->set_int(0, i, i);
    r->commit_transaction();

    auto track_changes = [&](auto&& f) {
        auto history = make_history(config.path);
#ifdef REALM_GROUP_SHARED_OPTIONS_HPP
        SharedGroup sg(*history, SharedGroupOptions(SharedGroupOptions::Durability::MemOnly));
#else
        SharedGroup sg(*history, SharedGroup::durability_MemOnly);
#endif
        Group const& g = sg.begin_read();

        r->begin_transaction();
        f();
        r->commit_transaction();

        _impl::TransactionChangeInfo info;
        info.table_modifications_needed.resize(g.size(), true);
        info.table_moves_needed.resize(g.size(), true);
        _impl::transaction::advance(sg, info);
        return info;
    };

    std::vector<_impl::DeepChangeChecker::RelatedTable> tables;
    _impl::DeepChangeChecker::find_related_tables(tables, *table);

    SECTION("direct changes are tracked") {
        auto info = track_changes([&] {
            table->set_int(0, 9, 10);
        });

        _impl::DeepChangeChecker checker(info, *table, tables);
        REQUIRE_FALSE(checker(8));
        REQUIRE(checker(9));
    }

    SECTION("changes over links are tracked") {
        r->begin_transaction();
        for (int i = 0; i < 9; ++i)
            table->set_link(1, i, i + 1);
        r->commit_transaction();

        auto info = track_changes([&] {
            table->set_int(0, 9, 10);
        });

        REQUIRE(_impl::DeepChangeChecker(info, *table, tables)(0));
    }

    SECTION("changes over linklists are tracked") {
        r->begin_transaction();
        for (int i = 0; i < 9; ++i)
            table->get_linklist(2, i)->add(i + 1);
        r->commit_transaction();

        auto info = track_changes([&] {
            table->set_int(0, 9, 10);
        });

        REQUIRE(_impl::DeepChangeChecker(info, *table, tables)(0));
    }

    SECTION("cycles over links do not loop forever") {
        r->begin_transaction();
        table->set_link(1, 0, 0);
        r->commit_transaction();

        auto info = track_changes([&] {
            table->set_int(0, 9, 10);
        });
        REQUIRE_FALSE(_impl::DeepChangeChecker(info, *table, tables)(0));
    }

    SECTION("cycles over linklists do not loop forever") {
        r->begin_transaction();
        table->get_linklist(2, 0)->add(0);
        r->commit_transaction();

        auto info = track_changes([&] {
            table->set_int(0, 9, 10);
        });
        REQUIRE_FALSE(_impl::DeepChangeChecker(info, *table, tables)(0));
    }

    SECTION("link chains are tracked up to 16 levels deep") {
        r->begin_transaction();
        table->add_empty_row(10);
        for (int i = 0; i < 19; ++i)
            table->set_link(1, i, i + 1);
        r->commit_transaction();

        auto info = track_changes([&] {
            table->set_int(0, 19, -1);
        });

        _impl::DeepChangeChecker checker(info, *table, tables);
        CHECK(checker(19));
        CHECK(checker(18));
        CHECK(checker(4));
        CHECK_FALSE(checker(3));
        CHECK_FALSE(checker(2));

        // Check in other orders to make sure that the caching doesn't effect
        // the results
        _impl::DeepChangeChecker checker2(info, *table, tables);
        CHECK_FALSE(checker2(2));
        CHECK_FALSE(checker2(3));
        CHECK(checker2(4));
        CHECK(checker2(18));
        CHECK(checker2(19));

        _impl::DeepChangeChecker checker3(info, *table, tables);
        CHECK(checker2(4));
        CHECK_FALSE(checker2(3));
        CHECK_FALSE(checker2(2));
        CHECK(checker2(18));
        CHECK(checker2(19));
    }

    SECTION("targets moving is not a change") {
        r->begin_transaction();
        table->set_link(1, 0, 9);
        table->get_linklist(2, 0)->add(9);
        r->commit_transaction();

        auto info = track_changes([&] {
            table->move_last_over(5);
        });
        REQUIRE_FALSE(_impl::DeepChangeChecker(info, *table, tables)(0));
    }

    SECTION("changes made before a row is moved are reported") {
        r->begin_transaction();
        table->set_link(1, 0, 9);
        r->commit_transaction();

        auto info = track_changes([&] {
            table->set_int(0, 9, 5);
            table->move_last_over(5);
        });
        REQUIRE(_impl::DeepChangeChecker(info, *table, tables)(0));

        r->begin_transaction();
        table->get_linklist(2, 0)->add(8);
        r->commit_transaction();

        info = track_changes([&] {
            table->set_int(0, 8, 5);
            table->move_last_over(5);
        });
        REQUIRE(_impl::DeepChangeChecker(info, *table, tables)(0));
    }

    SECTION("changes made after a row is moved are reported") {
        r->begin_transaction();
        table->set_link(1, 0, 9);
        r->commit_transaction();

        auto info = track_changes([&] {
            table->move_last_over(5);
            table->set_int(0, 5, 5);
        });
        REQUIRE(_impl::DeepChangeChecker(info, *table, tables)(0));

        r->begin_transaction();
        table->get_linklist(2, 0)->add(8);
        r->commit_transaction();

        info = track_changes([&] {
            table->move_last_over(5);
            table->set_int(0, 5, 5);
        });
        REQUIRE(_impl::DeepChangeChecker(info, *table, tables)(0));
    }
}
