# Scripting examples

## Simple postgres manager

It is easy and simple to setup (replicated) PostgreSQL using `PgManager`:

```lua
pgconfig = PgConf.new(conffile["default"])

pgm = PgManager.new(pgconfig)
pgm:setupAndStartPrimary()
pgm:setupAndStartAReplica()
-- can start additional replicas

primary = pgm:get(1)
replica = pgm:get(2)
-- ...
```

## Background threads

The following example shows the background thread functionality:

```lua
-- global variables shouldn't be used: the values can be different in every thread
var = "foo";

-- functions defined in lua can be called in any thread
function dep()
    info("From dep(): var=" .. var)
end

function bg_thread()
    info("From bgt()")
    dep()

    for i=1,10 do
        -- the receive, receiveIfAny and send functions are special to background threads, 
        -- and used to communication with the creator thread
        msg = receive();
        
        info("Received message: " .. msg);

        if msg == "exit" then
            break;
        end;
    end
end

function main()

    info("Starting background thread...")

    -- won't be reflected in the thread, do not depend on global state
    var = "bar"

    task = BackgroundThread.run("bglog", "bg_thread")

    info("While background thread running ... ")

    task:send("hey!")
    task:send("foo!")
    task:send("bar!")

    info("While background thread still running ... ")

    task:send("exit")

    task:join()

    info("All done")

end
```