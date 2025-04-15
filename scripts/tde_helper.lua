function init_pg_tde_only_for_db(sqlconn)
	sqlconn:execute_query("CREATE EXTENSION pg_tde;")
	sqlconn:execute_query("SET default_table_access_method = tde_heap;")
	sqlconn:execute_query("SELECT pg_tde_add_database_key_provider_file('reg_file', '/tmp/pg_tde_test_keyring.per');")
	sqlconn:execute_query("SELECT pg_tde_set_principal_key_using_database_key_provider('principal-key', 'reg_file');")
end

function init_pg_tde_globally(sqlconn)
	sqlconn:execute_query("CREATE EXTENSION pg_tde;")
	sqlconn:execute_query("SET default_table_access_method = tde_heap;")
	sqlconn:execute_query("SELECT pg_tde_add_global_key_provider_file('reg_file', '/tmp/pg_tde_test_keyring.per');")
	sqlconn:execute_query("SELECT pg_tde_set_principal_key_using_global_key_provider('server-principal-key', 'reg_file');")
	sqlconn:execute_query("SELECT pg_tde_set_default_principal_key_using_global_key_provider('def-principal-key', 'reg_file');")
	sqlconn:execute_query("ALTER SYSTEM SET pg_Tde.wal_encrypt = ON;")
end
