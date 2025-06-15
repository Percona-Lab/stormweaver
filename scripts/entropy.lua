require("lfs")

function calculate_entropy(filename)
	local file = assert(io.open(filename, "rb"))

	local page_size = 4 * 1024
	local byte_hist = {}
	local byte_hist_size = 256
	for i = 1, byte_hist_size do
		byte_hist[i] = 0
	end
	local total = 0

	repeat
		local str = file:read(page_size)
		for c in (str or ""):gmatch(".") do
			local byte = c:byte() + 1
			byte_hist[byte] = byte_hist[byte] + 1
			total = total + 1
		end
	until not str

	file:close()

	entropy = 0

	for _, count in ipairs(byte_hist) do
		if count ~= 0 then
			local p = 1.0 * count / total
			entropy = entropy - (p * math.log(p) / math.log(byte_hist_size))
		end
	end

	return entropy
end

function verify_entropy(relname, tablename, tableam, is_encrypted, filename)
	entropy = calculate_entropy(filename)
	info(
		"TABLE: "
			.. tablename
			.. " REL: "
			.. relname
			.. " FILE: "
			.. filename
			.. " AM: "
			.. tableam
			.. " IS_ENCRYPTED: "
			.. is_encrypted
			.. " ENTROPY: "
			.. entropy
	)
	if tableam == "tde_heap" then
		if is_encrypted ~= "t" then
			error("Table has tde_heap am but is not encrypted: " .. inspect(is_encrypted))
		end
		if entropy ~= 0 and entropy < 0.8 then
			warning("ENTROPY OF FILE " .. filename .. " FOR TABLE " .. tablename .. " is too low: " .. entropy)
		end
	else
		if is_encrypted == "t" then
			error("Table has heap am but is encrypted")
		end
		if entropy > 0.8 then
			warning(
				"FILE " .. filename .. " FOR TABLE " .. tablename .. " shouldn't be encrypted, but has high entropy!"
			)
		end
	end

	return entropy
end

function db_files_entropy(w, datadir)
	list_tables =
		"select pg_relation_filepath(c.oid), c.relname, a.amname, c.oid, pg_tde_is_encrypted(c.oid) from pg_class c join pg_am a ON a.oid = c.relam where relkind in ('r', 'm') AND c.relname not like 'pg_%' AND c.relname not like 'sql_%';"
	res = w:sql_connection():execute_query(list_tables)
	data = res:data()

	allData = {}
	for recId = 1, data:numRows() do
		table.insert(allData, data:nextRow())
	end

	for _, rec in ipairs(allData) do
		tableam = rec:field(3)
		tablename = rec:field(2)
		verify_entropy(tablename, tablename, tableam, rec:field(5), datadir .. rec:field(1))

		oid = rec:field(4)

		list_deps = "select pg_relation_filepath(c.oid), c.relname, pg_tde_is_encrypted(c.oid) from pg_class c join pg_attribute a on a.attrelid = "
			.. oid
			.. " join pg_depend d ON d.objid = c.oid and d.deptype in ('i', 'a') and a.attrelid = "
			.. oid
			.. " where d.refobjid="
			.. oid
			.. " group by c.oid"

		res2 = w:sql_connection():execute_query(list_deps)
		data2 = res2:data()
		for recId = 1, data2:numRows() do
			rec2 = data2:nextRow()

			if rec2:field(1) ~= nil then
				verify_entropy(rec2:field(2), tablename, tableam, rec2:field(3), datadir .. rec2:field(1))
			end
		end

		::continue::
	end
end
