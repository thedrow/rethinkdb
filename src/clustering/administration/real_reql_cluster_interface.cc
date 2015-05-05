// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/real_reql_cluster_interface.hpp"

#include "clustering/administration/artificial_reql_cluster_interface.hpp"
#include "clustering/administration/datum_adapter.hpp"
#include "clustering/administration/main/watchable_fields.hpp"
#include "clustering/administration/servers/config_client.hpp"
#include "clustering/administration/tables/generate_config.hpp"
#include "clustering/administration/tables/split_points.hpp"
#include "clustering/administration/tables/table_config.hpp"
#include "clustering/table_manager/table_meta_client.hpp"
#include "concurrency/cross_thread_signal.hpp"
#include "rdb_protocol/artificial_table/artificial_table.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/table_common.hpp"
#include "rdb_protocol/val.hpp"
#include "rpc/semilattice/watchable.hpp"
#include "rpc/semilattice/view/field.hpp"

#define NAMESPACE_INTERFACE_EXPIRATION_MS (60 * 1000)

real_reql_cluster_interface_t::real_reql_cluster_interface_t(
        mailbox_manager_t *_mailbox_manager,
        boost::shared_ptr<
            semilattice_readwrite_view_t<cluster_semilattice_metadata_t> > _semilattices,
        rdb_context_t *_rdb_context,
        server_config_client_t *_server_config_client,
        table_meta_client_t *_table_meta_client,
        watchable_map_t<
            std::pair<peer_id_t, std::pair<namespace_id_t, branch_id_t> >,
            table_query_bcard_t> *_table_query_directory
        ) :
    mailbox_manager(_mailbox_manager),
    semilattice_root_view(_semilattices),
    table_meta_client(_table_meta_client),
    cross_thread_database_watchables(get_num_threads()),
    rdb_context(_rdb_context),
    namespace_repo(
        mailbox_manager,
        _table_query_directory,
        rdb_context),
    changefeed_client(mailbox_manager,
        [this](const namespace_id_t &id, signal_t *interruptor) {
            return this->namespace_repo.get_namespace_interface(id, interruptor);
        }),
    server_config_client(_server_config_client)
{
    for (int thr = 0; thr < get_num_threads(); ++thr) {
        cross_thread_database_watchables[thr].init(
            new cross_thread_watchable_variable_t<databases_semilattice_metadata_t>(
                clone_ptr_t<semilattice_watchable_t<databases_semilattice_metadata_t> >
                    (new semilattice_watchable_t<databases_semilattice_metadata_t>(
                        metadata_field(&cluster_semilattice_metadata_t::databases, semilattice_root_view))), threadnum_t(thr)));
    }
}

bool real_reql_cluster_interface_t::db_create(const name_string_t &name,
        signal_t *interruptor, ql::datum_t *result_out, std::string *error_out) {
    guarantee(name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cluster_semilattice_metadata_t metadata;
    ql::datum_t new_config;
    {
        on_thread_t thread_switcher(semilattice_root_view->home_thread());
        metadata = semilattice_root_view->get();

        /* Make sure there isn't an existing database with the same name. */
        for (const auto &pair : metadata.databases.databases) {
            if (!pair.second.is_deleted() &&
                    pair.second.get_ref().name.get_ref() == name) {
                *error_out = strprintf("Database `%s` already exists.", name.c_str());
                return false;
            }
        }

        database_id_t db_id = generate_uuid();
        database_semilattice_metadata_t db;
        db.name = versioned_t<name_string_t>(name);
        metadata.databases.databases.insert(std::make_pair(db_id, make_deletable(db)));

        semilattice_root_view->join(metadata);
        metadata = semilattice_root_view->get();

        new_config = convert_db_config_and_name_to_datum(name, db_id);
    }
    wait_for_metadata_to_propagate(metadata, interruptor);

    ql::datum_object_builder_t result_builder;
    result_builder.overwrite("dbs_created", ql::datum_t(1.0));
    result_builder.overwrite("config_changes",
        make_replacement_pair(ql::datum_t::null(), new_config));
    *result_out = std::move(result_builder).to_datum();

    return true;
}

bool real_reql_cluster_interface_t::db_drop(const name_string_t &name,
        signal_t *interruptor, ql::datum_t *result_out, std::string *error_out) {
    guarantee(name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cluster_semilattice_metadata_t metadata;
    ql::datum_t old_config;
    size_t tables_dropped;
    {
        cross_thread_signal_t interruptor2(interruptor,
            semilattice_root_view->home_thread());
        on_thread_t thread_switcher(semilattice_root_view->home_thread());
        metadata = semilattice_root_view->get();
        database_id_t db_id;
        if (!search_db_metadata_by_name(metadata.databases, name, &db_id, error_out)) {
            return false;
        }

        /* Delete all of the tables in the database */
        std::map<namespace_id_t, table_basic_config_t> tables;
        table_meta_client->list_names(&tables);
        tables_dropped = 0;
        for (const auto &pair : tables) {
            if (pair.second.database == db_id) {
                try {
                    table_meta_client->drop(pair.first, &interruptor2);
                    ++tables_dropped;
                } catch (const no_such_table_exc_t &) {
                    /* The table was dropped by something else between the time when we
                    called `list_names()` and when we went to actually delete it. This is
                    OK. */
                } CATCH_OP_ERRORS(name, pair.second.name, error_out,
                    "The database was not dropped, but some of the tables in it may or "
                        "may not have been dropped.",
                    "The database was not dropped, but some of the tables in it may or "
                        "may not have been dropped.")
            }
        }

        old_config = convert_db_config_and_name_to_datum(name, db_id);

        metadata.databases.databases.at(db_id).mark_deleted();

        semilattice_root_view->join(metadata);
        metadata = semilattice_root_view->get();

    }
    wait_for_metadata_to_propagate(metadata, interruptor);

    ql::datum_object_builder_t result_builder;
    result_builder.overwrite("dbs_dropped", ql::datum_t(1.0));
    result_builder.overwrite("tables_dropped",
        ql::datum_t(static_cast<double>(tables_dropped)));
    result_builder.overwrite("config_changes",
        make_replacement_pair(old_config, ql::datum_t::null()));
    *result_out = std::move(result_builder).to_datum();

    return true;
}

bool real_reql_cluster_interface_t::db_list(
        UNUSED signal_t *interruptor, std::set<name_string_t> *names_out,
        UNUSED std::string *error_out) {
    databases_semilattice_metadata_t db_metadata;
    get_databases_metadata(&db_metadata);
    for (const auto &pair : db_metadata.databases) {
        if (!pair.second.is_deleted()) {
            names_out->insert(pair.second.get_ref().name.get_ref());
        }
    }
    return true;
}

bool real_reql_cluster_interface_t::db_find(const name_string_t &name,
        UNUSED signal_t *interruptor, counted_t<const ql::db_t> *db_out,
        std::string *error_out) {
    guarantee(name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    /* Find the specified database */
    databases_semilattice_metadata_t db_metadata;
    get_databases_metadata(&db_metadata);
    database_id_t db_id;
    if (!search_db_metadata_by_name(db_metadata, name, &db_id, error_out)) {
        return false;
    }
    *db_out = make_counted<const ql::db_t>(db_id, name);
    return true;
}

bool real_reql_cluster_interface_t::db_config(
        const counted_t<const ql::db_t> &db,
        ql::backtrace_id_t bt,
        ql::env_t *env,
        scoped_ptr_t<ql::val_t> *selection_out,
        std::string *error_out) {
    try {
        make_single_selection(admin_tables->db_config_backend.get(),
            name_string_t::guarantee_valid("db_config"), db->id, bt, env,
            selection_out);
        return true;
    } catch (const no_such_table_exc_t &) {
        *error_out = strprintf("Database `%s` does not exist.", db->name.c_str());
        return false;
    } catch (const admin_op_exc_t &msg) {
        *error_out = msg.what();
        return false;
    }
}

bool real_reql_cluster_interface_t::table_create(const name_string_t &name,
        counted_t<const ql::db_t> db,
        const table_generate_config_params_t &config_params,
        const std::string &primary_key,
        write_durability_t durability,
        signal_t *interruptor, ql::datum_t *result_out, std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");

    namespace_id_t table_id;
    cluster_semilattice_metadata_t metadata;
    ql::datum_t new_config;
    try {
        cross_thread_signal_t interruptor2(interruptor,
            semilattice_root_view->home_thread());
        on_thread_t thread_switcher(semilattice_root_view->home_thread());

        /* Make sure there isn't an existing table with the same name */
        if (table_meta_client->exists(db->id, name)) {
            *error_out = strprintf("Table `%s.%s` already exists.", db->name.c_str(),
                name.c_str());
            return false;
        }

        table_config_and_shards_t config;

        config.config.basic.name = name;
        config.config.basic.database = db->id;
        config.config.basic.primary_key = primary_key;

        /* We don't have any data to generate split points based on, so assume UUIDs */
        calculate_split_points_for_uuids(
            config_params.num_shards, &config.shard_scheme);

        /* Pick which servers to host the data */
        table_generate_config(
            server_config_client, nil_uuid(), table_meta_client,
            config_params, config.shard_scheme, &interruptor2,
            &config.config.shards);

        config.config.write_ack_config.mode = write_ack_config_t::mode_t::majority;
        config.config.durability = durability;

        table_id = generate_uuid();
        table_meta_client->create(table_id, config, &interruptor2);

        new_config = convert_table_config_to_datum(table_id,
            convert_name_to_datum(db->name), config.config,
            admin_identifier_format_t::name, server_config_client);

        table_status_artificial_table_backend_t *status_backend =
            admin_tables->table_status_backend[
                static_cast<int>(admin_identifier_format_t::name)].get();
        wait_for_table_readiness(
            table_id,
            table_readiness_t::finished,
            status_backend,
            &interruptor2,
            nullptr);

        ql::datum_object_builder_t result_builder;
        result_builder.overwrite("tables_created", ql::datum_t(1.0));
        result_builder.overwrite("config_changes",
            make_replacement_pair(ql::datum_t::null(), new_config));
        *result_out = std::move(result_builder).to_datum();

        return true;

    } catch (const admin_op_exc_t &msg) {
        *error_out = msg.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
      CATCH_OP_ERRORS(db->name, name, error_out,
        "The table was not created.",
        "The table may or may not have been created.")
}

bool real_reql_cluster_interface_t::table_drop(const name_string_t &name,
        counted_t<const ql::db_t> db, signal_t *interruptor, ql::datum_t *result_out,
        std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cluster_semilattice_metadata_t metadata;
    ql::datum_t old_config;
    try {
        on_thread_t thread_switcher(semilattice_root_view->home_thread());
        metadata = semilattice_root_view->get();

        namespace_id_t table_id;
        table_meta_client->find(db->id, name, &table_id);

        table_config_and_shards_t config;
        table_meta_client->get_config(table_id, interruptor, &config);

        old_config = convert_table_config_to_datum(table_id,
            convert_name_to_datum(db->name), config.config,
            admin_identifier_format_t::name, server_config_client);

        table_meta_client->drop(table_id, interruptor);

        ql::datum_object_builder_t result_builder;
        result_builder.overwrite("tables_dropped", ql::datum_t(1.0));
        result_builder.overwrite("config_changes",
            make_replacement_pair(old_config, ql::datum_t::null()));
        *result_out = std::move(result_builder).to_datum();

        return true;

    } CATCH_NAME_ERRORS(db->name, name, error_out)
      CATCH_OP_ERRORS(db->name, name, error_out,
        "The table was not dropped.",
        "The table may or may not have been dropped.")
}

bool real_reql_cluster_interface_t::table_list(counted_t<const ql::db_t> db,
        UNUSED signal_t *interruptor, std::set<name_string_t> *names_out,
        UNUSED std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    std::map<namespace_id_t, table_basic_config_t> tables;
    table_meta_client->list_names(&tables);
    for (const auto &pair : tables) {
        if (pair.second.database == db->id) {
            names_out->insert(pair.second.name);
        }
    }
    return true;
}

bool real_reql_cluster_interface_t::table_find(
        const name_string_t &name, counted_t<const ql::db_t> db,
        UNUSED boost::optional<admin_identifier_format_t> identifier_format,
        signal_t *interruptor,
        counted_t<base_table_t> *table_out, std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    namespace_id_t table_id;
    std::string primary_key;
    try {
        table_meta_client->find(db->id, name, &table_id, &primary_key);

        /* Note that we completely ignore `identifier_format`. `identifier_format` is
        meaningless for real tables, so it might seem like we should produce an error.
        The reason we don't is that the user might write a query that access both a
        system table and a real table, and they might specify `identifier_format` as a
        global optarg. So then they would get a spurious error for the real table. This
        behavior is also consistent with that of system tables that aren't affected by
        `identifier_format`. */
        table_out->reset(new real_table_t(
            table_id,
            namespace_repo.get_namespace_interface(table_id, interruptor),
            primary_key,
            &changefeed_client));

        return true;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
}

bool real_reql_cluster_interface_t::table_estimate_doc_counts(
        counted_t<const ql::db_t> db,
        const name_string_t &name,
        ql::env_t *env,
        std::vector<int64_t> *doc_counts_out,
        std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cross_thread_signal_t interruptor2(env->interruptor,
        semilattice_root_view->home_thread());

    try {
        on_thread_t thread_switcher(semilattice_root_view->home_thread());

        namespace_id_t table_id;
        table_meta_client->find(db->id, name, &table_id);

        table_config_and_shards_t config;
        table_meta_client->get_config(table_id, &interruptor2, &config);

        /* Perform a distribution query against the database */
        std::map<store_key_t, int64_t> counts;
        fetch_distribution(table_id, this, &interruptor2, &counts);

        /* Match the results of the distribution query against the table's shard
        boundaries */
        *doc_counts_out = std::vector<int64_t>(config.shard_scheme.num_shards(), 0);
        for (auto it = counts.begin(); it != counts.end(); ++it) {
            /* Calculate the range of shards that this key-range overlaps with */
            size_t left_shard = config.shard_scheme.find_shard_for_key(it->first);
            auto jt = it;
            ++jt;
            size_t right_shard;
            if (jt == counts.end()) {
                right_shard = config.shard_scheme.num_shards() - 1;
            } else {
                store_key_t right_key = jt->first;
                bool ok = right_key.decrement();
                guarantee(ok, "jt->first cannot be the leftmost key");
                right_shard = config.shard_scheme.find_shard_for_key(right_key);
            }
            /* We assume that every shard that this key-range overlaps with has an equal
            share of the keys in the key-range. This is shitty but oh well. */
            for (size_t shard = left_shard; shard <= right_shard; ++shard) {
                doc_counts_out->at(shard) += it->second / (right_shard - left_shard + 1);
            }
        }
        return true;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
      CATCH_OP_ERRORS(db->name, name, error_out, "", "")
}

bool real_reql_cluster_interface_t::table_config(
        counted_t<const ql::db_t> db,
        const name_string_t &name,
        ql::backtrace_id_t bt,
        ql::env_t *env,
        scoped_ptr_t<ql::val_t> *selection_out,
        std::string *error_out) {
    try {
        namespace_id_t table_id;
        table_meta_client->find(db->id, name, &table_id);
        make_single_selection(
            admin_tables->table_config_backend[
                static_cast<int>(admin_identifier_format_t::name)].get(),
            name_string_t::guarantee_valid("table_config"), table_id, bt,
            env, selection_out);
        return true;
    } catch (const admin_op_exc_t &msg) {
        *error_out = msg.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
}

bool real_reql_cluster_interface_t::table_status(
        counted_t<const ql::db_t> db,
        const name_string_t &name,
        ql::backtrace_id_t bt,
        ql::env_t *env,
        scoped_ptr_t<ql::val_t> *selection_out,
        std::string *error_out) {
    try {
        namespace_id_t table_id;
        table_meta_client->find(db->id, name, &table_id);
        make_single_selection(
            admin_tables->table_status_backend[
                static_cast<int>(admin_identifier_format_t::name)].get(),
            name_string_t::guarantee_valid("table_status"), table_id, bt,
            env, selection_out);
        return true;
    } catch (const admin_op_exc_t &msg) {
        *error_out = msg.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
}

/* Waits until all of the tables listed in `tables` are ready to the given level of
readiness, or have been deleted. */
void real_reql_cluster_interface_t::wait_internal(
        std::set<namespace_id_t> tables,
        table_readiness_t readiness,
        signal_t *interruptor,
        ql::datum_t *result_out,
        int *count_out)
        THROWS_ONLY(interrupted_exc_t, admin_op_exc_t) {
    table_status_artificial_table_backend_t *status_backend =
        admin_tables->table_status_backend[
            static_cast<int>(admin_identifier_format_t::name)].get();

    /* Record the old values of `table_status` so we can report them in the result */
    std::map<namespace_id_t, ql::datum_t> old_statuses;
    for (auto it = tables.begin(); it != tables.end();) {
        ql::datum_t status;
        std::string error;
        if (!status_backend->read_row(
                convert_uuid_to_datum(*it), interruptor, &status, &error)) {
            throw admin_op_exc_t(error);
        }
        if (!status.has()) {
            /* The table was deleted; remove it from the set */
            auto jt = it;
            ++jt;
            tables.erase(it);
            it = jt;
        } else {
            old_statuses[*it] = status;
            ++it;
        }
    }

    std::map<namespace_id_t, ql::datum_t> new_statuses;
    /* We alternate between two checks for readiness: waiting on `table_status_backend`,
    and running test queries. We consider ourselves done when both tests succeed for
    every table with no failures in between. */
    while (true) {
        /* First we wait until all the `table_status_backend` checks succeed in a row */
        {
            threadnum_t new_thread = status_backend->home_thread();
            cross_thread_signal_t ct_interruptor(interruptor, new_thread);
            on_thread_t thread_switcher(new_thread);

            // Loop until all tables are ready - we have to check all tables again
            // if a table was not immediately ready, because we're supposed to
            // wait until all the checks succeed with no failures in between.
            bool immediate;
            do {
                new_statuses.clear();
                immediate = true;
                for (auto it = tables.begin(); it != tables.end();) {
                    ql::datum_t status;
                    table_wait_result_t res = wait_for_table_readiness(
                        *it, readiness, status_backend, &ct_interruptor, &status);
                    if (res == table_wait_result_t::DELETED) {
                        /* Remove this entry so we don't keep trying to wait on it after
                        it's been erased */
                        tables.erase(it++);
                    } else {
                        new_statuses[*it] = status;
                        ++it;
                    }
                    immediate = immediate && (res == table_wait_result_t::IMMEDIATE);
                }
            } while (!immediate && tables.size() != 1);
        }

        /* Next we check that test queries succeed. */
        // We cannot wait for anything higher than 'writes' through namespace_interface_t
        table_readiness_t readiness2 = std::min(readiness, table_readiness_t::writes);
        bool success = true;
        for (auto it = tables.begin(); it != tables.end() && success; ++it) {
            namespace_interface_access_t ns_if =
                namespace_repo.get_namespace_interface(*it, interruptor);
            success = success && ns_if.get()->check_readiness(readiness2, interruptor);
        }
        if (success) {
            break;
        }

        /* The `table_status` check succeeded, but the test query didn't. Go back and try
        the `table_status` check again, after waiting for a bit. */
        nap(100, interruptor);
    }

    guarantee(new_statuses.size() == tables.size());

    if (result_out != nullptr) {
        ql::datum_object_builder_t result_builder;
        result_builder.overwrite("ready",
            ql::datum_t(static_cast<double>(tables.size())));
        ql::datum_array_builder_t status_changes_builder(
            ql::configured_limits_t::unlimited);
        for (const namespace_id_t &table_id : tables) {
            ql::datum_object_builder_t change_builder;
            change_builder.overwrite("old_val", old_statuses.at(table_id));
            change_builder.overwrite("new_val", new_statuses.at(table_id));
            status_changes_builder.add(std::move(change_builder).to_datum());
        }
        result_builder.overwrite("status_changes",
            std::move(status_changes_builder).to_datum());
        *result_out = std::move(result_builder).to_datum();
    }
    if (count_out != nullptr) {
        *count_out = tables.size();
    }
}

bool real_reql_cluster_interface_t::table_wait(
        counted_t<const ql::db_t> db,
        const name_string_t &name,
        table_readiness_t readiness,
        signal_t *interruptor,
        ql::datum_t *result_out,
        std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    try {
        namespace_id_t table_id;
        table_meta_client->find(db->id, name, &table_id);

        int num_waited;
        wait_internal({table_id}, readiness, interruptor, result_out, &num_waited);

        /* If the table is deleted, then `wait_internal()` will just return normally.
        This behavior makes sense for `db_wait()` but not for `table_wait()`. So we
        manually check to see if the wait succeeded. */
        if (num_waited != 1) {
            throw no_such_table_exc_t();
        }

        return true;
    } catch (const admin_op_exc_t &msg) {
        *error_out = msg.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
}

bool real_reql_cluster_interface_t::db_wait(
        counted_t<const ql::db_t> db,
        table_readiness_t readiness,
        signal_t *interruptor,
        ql::datum_t *result_out,
        std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");

    std::set<namespace_id_t> table_ids;
    std::map<namespace_id_t, table_basic_config_t> tables;
    table_meta_client->list_names(&tables);
    for (const auto &table : tables) {
        if (table.second.database == db->id) {
            table_ids.insert(table.first);
        }
    }

    try {
        wait_internal(table_ids, readiness, interruptor, result_out, nullptr);
        return true;
    } catch (const admin_op_exc_t &msg) {
        *error_out = msg.what();
        return false;
    }
}

void real_reql_cluster_interface_t::reconfigure_internal(
        const counted_t<const ql::db_t> &db,
        const namespace_id_t &table_id,
        const table_generate_config_params_t &params,
        bool dry_run,
        signal_t *interruptor,
        ql::datum_t *result_out)
        THROWS_ONLY(interrupted_exc_t, no_such_table_exc_t, admin_op_exc_t,
            failed_table_op_exc_t, maybe_failed_table_op_exc_t) {
    rassert(get_thread_id() == server_config_client->home_thread());

    /* Fetch the table's current configuration */
    table_config_and_shards_t old_config;
    table_meta_client->get_config(table_id, interruptor, &old_config);

    // Store the old value of the config and status
    ql::datum_t old_config_datum = convert_table_config_to_datum(
        table_id, convert_name_to_datum(db->name), old_config.config,
        admin_identifier_format_t::name, server_config_client);

    table_status_artificial_table_backend_t *status_backend =
        admin_tables->table_status_backend[
            static_cast<int>(admin_identifier_format_t::name)].get();
    ql::datum_t old_status;
    std::string error;
    if (!status_backend->read_row(convert_uuid_to_datum(table_id), interruptor,
            &old_status, &error)) {
        throw admin_op_exc_t(error);
    }

    table_config_and_shards_t new_config;
    new_config.config.basic = old_config.config.basic;

    calculate_split_points_intelligently(
        table_id,
        this,
        params.num_shards,
        old_config.shard_scheme,
        interruptor,
        &new_config.shard_scheme);

    /* `table_generate_config()` just generates the config; it doesn't apply it */
    table_generate_config(
        server_config_client, table_id, table_meta_client,
        params, new_config.shard_scheme, interruptor, &new_config.config.shards);

    new_config.config.write_ack_config.mode = write_ack_config_t::mode_t::majority;
    new_config.config.durability = write_durability_t::HARD;

    if (!dry_run) {
        table_meta_client->set_config(table_id, new_config, interruptor);
    }

    // Compute the new value of the config and status
    ql::datum_t new_config_datum = convert_table_config_to_datum(
        table_id, convert_name_to_datum(db->name), new_config.config,
        admin_identifier_format_t::name, server_config_client);
    ql::datum_t new_status;
    if (!status_backend->read_row(convert_uuid_to_datum(table_id), interruptor,
            &new_status, &error)) {
        throw admin_op_exc_t(error);
    }

    ql::datum_object_builder_t result_builder;
    if (!dry_run) {
        result_builder.overwrite("reconfigured", ql::datum_t(1.0));
        result_builder.overwrite("config_changes",
            make_replacement_pair(old_config_datum, new_config_datum));
        result_builder.overwrite("status_changes",
            make_replacement_pair(old_status, new_status));
    } else {
        result_builder.overwrite("reconfigured", ql::datum_t(0.0));
        result_builder.overwrite("config_changes",
            make_replacement_pair(old_config_datum, new_config_datum));
    }
    *result_out = std::move(result_builder).to_datum();
}

bool real_reql_cluster_interface_t::table_reconfigure(
        counted_t<const ql::db_t> db,
        const name_string_t &name,
        const table_generate_config_params_t &params,
        bool dry_run,
        signal_t *interruptor,
        ql::datum_t *result_out,
        std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cross_thread_signal_t ct_interruptor(interruptor,
        server_config_client->home_thread());
    try {
        on_thread_t thread_switcher(server_config_client->home_thread());
        namespace_id_t table_id;
        table_meta_client->find(db->id, name, &table_id);
        reconfigure_internal(db, table_id, params, dry_run, &ct_interruptor, result_out);
        return true;
    } catch (const admin_op_exc_t &msg) {
        *error_out = msg.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
      CATCH_OP_ERRORS(db->name, name, error_out,
        "The table was not reconfigured.",
        "The table may or may not have been reconfigured.")
}

bool real_reql_cluster_interface_t::db_reconfigure(
        counted_t<const ql::db_t> db,
        const table_generate_config_params_t &params,
        bool dry_run,
        signal_t *interruptor,
        ql::datum_t *result_out,
        std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cross_thread_signal_t ct_interruptor(interruptor,
        server_config_client->home_thread());
    on_thread_t thread_switcher(server_config_client->home_thread());

    std::map<namespace_id_t, table_basic_config_t> tables;
    table_meta_client->list_names(&tables);

    ql::datum_t combined_stats = ql::datum_t::empty_object();
    for (const auto &pair : tables) {
        if (pair.second.database != db->id) {
            continue;
        }
        ql::datum_t stats;
        try {
            reconfigure_internal(
                db, pair.first, params, dry_run, &ct_interruptor, &stats);
        } catch (const no_such_table_exc_t &) {
            /* The table got deleted during the reconfiguration. It would be weird if
            `r.db('foo').reconfigure()` produced an error complaining that some table
            `foo.bar` did not exist. So we just skip the table, as though it were
            deleted before the operation even began. */
            continue;
        } catch (const admin_op_exc_t &msg) {
            *error_out = msg.what();
            return false;
        } CATCH_OP_ERRORS(db->name, pair.second.name, error_out,
            "The tables may or may not have been reconfigured.",
            "The tables may or may not have been reconfigured.")
        std::set<std::string> dummy_conditions;
        combined_stats = combined_stats.merge(stats, &ql::stats_merge,
            ql::configured_limits_t::unlimited, &dummy_conditions);
        guarantee(dummy_conditions.empty());
    }
    *result_out = combined_stats;
    return true;
}

void real_reql_cluster_interface_t::rebalance_internal(
        const namespace_id_t &table_id,
        signal_t *interruptor,
        ql::datum_t *results_out)
        THROWS_ONLY(interrupted_exc_t, no_such_table_exc_t,
            failed_table_op_exc_t, maybe_failed_table_op_exc_t, admin_op_exc_t) {

    /* Fetch the table's current configuration */
    table_config_and_shards_t config;
    table_meta_client->get_config(table_id, interruptor, &config);

    table_status_artificial_table_backend_t *status_backend =
        admin_tables->table_status_backend[
            static_cast<int>(admin_identifier_format_t::name)].get();
    ql::datum_t old_status;
    std::string error;
    if (!status_backend->read_row(convert_uuid_to_datum(table_id), interruptor,
            &old_status, &error)) {
        throw admin_op_exc_t(error);
    }

    std::map<store_key_t, int64_t> counts;
    fetch_distribution(table_id, this, interruptor, &counts);

    /* If there's not enough data to rebalance, return `rebalanced: 0` but don't report
    an error */
    bool actually_rebalanced = calculate_split_points_with_distribution(
        counts, config.config.shards.size(), &config.shard_scheme);
    if (actually_rebalanced) {
        table_meta_client->set_config(table_id, config, interruptor);
    }

    ql::datum_t new_status;
    if (!status_backend->read_row(convert_uuid_to_datum(table_id), interruptor,
            &new_status, &error)) {
        throw admin_op_exc_t(error);
    }

    ql::datum_object_builder_t builder;
    builder.overwrite("rebalanced", ql::datum_t(actually_rebalanced ? 1.0 : 0.0));
    builder.overwrite("status_changes", make_replacement_pair(old_status, new_status));
    *results_out = std::move(builder).to_datum();
}

bool real_reql_cluster_interface_t::table_rebalance(
        counted_t<const ql::db_t> db,
        const name_string_t &name,
        signal_t *interruptor,
        ql::datum_t *result_out,
        std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cross_thread_signal_t ct_interruptor(interruptor,
        server_config_client->home_thread());
    try {
        on_thread_t thread_switcher(server_config_client->home_thread());
        namespace_id_t table_id;
        table_meta_client->find(db->id, name, &table_id);
        rebalance_internal(table_id, &ct_interruptor, result_out);
        return true;
    } catch (const admin_op_exc_t &msg) {
        *error_out = msg.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
      CATCH_OP_ERRORS(db->name, name, error_out,
        "The table was not rebalanced.",
        "The table may or may not have been rebalanced.")
}

bool real_reql_cluster_interface_t::db_rebalance(
        counted_t<const ql::db_t> db,
        signal_t *interruptor,
        ql::datum_t *result_out,
        std::string *error_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cross_thread_signal_t ct_interruptor(interruptor,
        server_config_client->home_thread());
    on_thread_t thread_switcher(server_config_client->home_thread());

    std::map<namespace_id_t, table_basic_config_t> tables;
    table_meta_client->list_names(&tables);

    ql::datum_t combined_stats = ql::datum_t::empty_object();
    for (const auto &pair : tables) {
        if (pair.second.database != db->id) {
            continue;
        }
        ql::datum_t stats;
        try {
            rebalance_internal(pair.first, &ct_interruptor, &stats);
        } catch (const no_such_table_exc_t &) {
            /* This table was deleted while we were iterating over the tables list. So
            just ignore it to avoid making a confusing error message. */
            continue;
        } catch (const admin_op_exc_t &msg) {
            *error_out = msg.what();
            return false;
        } CATCH_OP_ERRORS(db->name, pair.second.name, error_out,
            "The tables may or may not have been rebalanced.",
            "The tables may or may not have been rebalanced.")
        std::set<std::string> dummy_conditions;
        combined_stats = combined_stats.merge(stats, &ql::stats_merge,
            ql::configured_limits_t::unlimited, &dummy_conditions);
        guarantee(dummy_conditions.empty());
    }
    *result_out = combined_stats;
    return true;
}

void real_reql_cluster_interface_t::sindex_change_internal(
        const counted_t<const ql::db_t> &db,
        const name_string_t &table_name,
        const std::function<void(std::map<std::string, sindex_config_t> *)> &cb,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t, no_such_table_exc_t,
            failed_table_op_exc_t, maybe_failed_table_op_exc_t){
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cross_thread_signal_t ct_interruptor(interruptor,
        server_config_client->home_thread());
    on_thread_t thread_switcher(server_config_client->home_thread());
    namespace_id_t table_id;
    table_meta_client->find(db->id, table_name, &table_id);
    table_config_and_shards_t config;
    table_meta_client->get_config(table_id, interruptor, &config);
    cb(&config.config.sindexes);
    table_meta_client->set_config(table_id, config, interruptor);
}

bool real_reql_cluster_interface_t::sindex_create(
        counted_t<const ql::db_t> db,
        const name_string_t &table,
        const std::string &name,
        const sindex_config_t &config,
        signal_t *interruptor,
        std::string *error_out) {
    try {
        sindex_change_internal(
            db, table,
            [&](std::map<std::string, sindex_config_t> *map) {
                if (map->count(name) == 1) {
                    throw admin_op_exc_t(strprintf(
                        "Index `%s` already exists on table `%s.%s`.",
                        name.c_str(), db->name.c_str(), table.c_str()));
                }
                map->insert(std::make_pair(name, config));
            },
            interruptor);
        return true;
    } catch (const admin_op_exc_t &exc) {
        *error_out = exc.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
      CATCH_OP_ERRORS(db->name, name, error_out,
        "The secondary index was not created.",
        "The secondary index may or may not have been created.")
}

bool real_reql_cluster_interface_t::sindex_drop(
        counted_t<const ql::db_t> db,
        const name_string_t &table,
        const std::string &name,
        signal_t *interruptor,
        std::string *error_out) {
    try {
        sindex_change_internal(
            db, table,
            [&](std::map<std::string, sindex_config_t> *map) {
                if (map->count(name) == 0) {
                    throw admin_op_exc_t(strprintf(
                        "Index `%s` does not exist on table `%s.%s`.",
                        name.c_str(), db->name.c_str(), table.c_str()));
                }
                map->erase(name);
            },
            interruptor);
        return true;
    } catch (const admin_op_exc_t &exc) {
        *error_out = exc.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
      CATCH_OP_ERRORS(db->name, name, error_out,
        "The secondary index was not dropped.",
        "The secondary index may or may not have been dropped.")
}

bool real_reql_cluster_interface_t::sindex_rename(
        counted_t<const ql::db_t> db,
        const name_string_t &table,
        const std::string &name,
        const std::string &new_name,
        bool overwrite,
        signal_t *interruptor,
        std::string *error_out) {
    try {
        sindex_change_internal(
            db, table,
            [&](std::map<std::string, sindex_config_t> *map) {
                if (map->count(name) == 0) {
                    throw admin_op_exc_t(strprintf(
                        "Index `%s` does not exist on table `%s.%s`.",
                        name.c_str(), db->name.c_str(), table.c_str()));
                }
                if (map->count(new_name) == 1) {
                    if (overwrite) {
                        map->erase(new_name);
                    } else {
                        throw admin_op_exc_t(strprintf(
                            "Index `%s` already exists on table `%s.%s`.",
                            new_name.c_str(), db->name.c_str(), table.c_str()));
                    }
                }
                sindex_config_t config = map->at(name);
                map->erase(name);
                map->insert(std::make_pair(new_name, config));
            },
            interruptor);
        return true;
    } catch (const admin_op_exc_t &exc) {
        *error_out = exc.what();
        return false;
    } CATCH_NAME_ERRORS(db->name, name, error_out)
      CATCH_OP_ERRORS(db->name, name, error_out,
        "The secondary index was not renamed.",
        "The secondary index may or may not have been renamed.")
}

bool real_reql_cluster_interface_t::sindex_list(
        counted_t<const ql::db_t> db,
        const name_string_t &table_name,
        signal_t *interruptor,
        std::string *error_out,
        std::map<std::string, std::pair<sindex_config_t, sindex_status_t> >
            *configs_and_statuses_out) {
    guarantee(db->name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cross_thread_signal_t ct_interruptor(interruptor,
        server_config_client->home_thread());
    try {
        on_thread_t thread_switcher(server_config_client->home_thread());
        namespace_id_t table_id;
        table_meta_client->find(db->id, table_name, &table_id);
        table_meta_client->get_status(
            table_id, &ct_interruptor, configs_and_statuses_out, nullptr);
        return true;
    } CATCH_NAME_ERRORS(db->name, table_name, error_out)
      CATCH_OP_ERRORS(db->name, table_name, error_out, "", "")
}

/* Checks that divisor is indeed a divisor of multiple. */
template <class T>
bool is_joined(const T &multiple, const T &divisor) {
    T cpy = multiple;

    semilattice_join(&cpy, divisor);
    return cpy == multiple;
}

void real_reql_cluster_interface_t::wait_for_metadata_to_propagate(
        const cluster_semilattice_metadata_t &metadata, signal_t *interruptor) {
    int threadnum = get_thread_id().threadnum;

    guarantee(cross_thread_database_watchables[threadnum].has());
    cross_thread_database_watchables[threadnum]->get_watchable()->run_until_satisfied(
            [&] (const databases_semilattice_metadata_t &md) -> bool
                { return is_joined(md, metadata.databases); },
            interruptor);
}

template <class T>
void copy_value(const T *in, T *out) {
    *out = *in;
}

void real_reql_cluster_interface_t::get_databases_metadata(
        databases_semilattice_metadata_t *out) {
    int threadnum = get_thread_id().threadnum;
    r_sanity_check(cross_thread_database_watchables[threadnum].has());
    cross_thread_database_watchables[threadnum]->apply_read(
            std::bind(&copy_value<databases_semilattice_metadata_t>,
                      ph::_1, out));
}

void real_reql_cluster_interface_t::make_single_selection(
        artificial_table_backend_t *table_backend,
        const name_string_t &table_name,
        const uuid_u &primary_key,
        ql::backtrace_id_t bt,
        ql::env_t *env,
        scoped_ptr_t<ql::val_t> *selection_out)
        THROWS_ONLY(no_such_table_exc_t, admin_op_exc_t) {
    ql::datum_t row;
    std::string error;
    if (!table_backend->read_row(convert_uuid_to_datum(primary_key), env->interruptor,
            &row, &error)) {
        throw admin_op_exc_t(error);
    }
    if (!row.has()) {
        /* This is unlikely, but it can happen if the object is deleted between when we
        look up its name and when we call `read_row()` */
        throw no_such_table_exc_t();
    }
    counted_t<ql::table_t> table = make_counted<ql::table_t>(
        counted_t<base_table_t>(new artificial_table_t(table_backend)),
        make_counted<const ql::db_t>(
            nil_uuid(), name_string_t::guarantee_valid("rethinkdb")),
        table_name.str(), false, bt);
    *selection_out = make_scoped<ql::val_t>(
        ql::single_selection_t::from_row(env, bt, table, row),
        bt);
}

