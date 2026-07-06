[settings]
compiler.cppstd=gnu23

[conf]
# gcc >= 15 defaults C to C23; libpq's typedef of bool breaks under C23
tools.build:cflags=["-std=gnu17"]
