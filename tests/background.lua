local var = "foo"

function dep()
	info("From dep(): " .. var)
end

function bgt()
	info("From bgt()")
	dep()

	for i = 1, 10 do
		msg = receive()

		info("Received message: " .. msg)

		if msg == "exit" then
			break
		end
	end
end

function main()
	info("Starting background thread...")

	-- won't be reflected in the thread, do not depend on global state
	var = "bar"

	task = BackgroundThread.run("bglog", "bgt")

	info("While background thread running ... ")

	task:send("hey!")
	task:send("foo!")
	task:send("bar!")

	info("While background thread still running ... ")

	task:send("exit")

	task:join()

	info("All done")
end
