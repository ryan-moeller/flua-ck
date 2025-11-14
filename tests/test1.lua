local ck = require('ck')
local const = ck.shared.const

local f = const.new(function() print('f') end)
f:load()()

local x = 123
local f1 = const.new(function() x = x + 1; return x end)
local a = f1:load()()
x = 10
local b = f1:load()()
assert(a == 124)
assert(b == 124)
local f3 = f1:load()
assert(f3() == 124)
assert(f3() == 125)

local ref = const.retain(f:cookie())
ref:load()()

local mut = ck.shared.mut

local mf = mut.new(function()
	print(message)
end)
local printer = mf:load()
printer()
message = "hello world"
printer()
mf:store(function(prefix)
	if prefix then
		print(prefix, message)
	else
		print(message)
	end
end)
local ref = mut.retain(mf:cookie())
local printer1 = printer
local printer = ref:load()
printer()
printer("message:")
message = "goodbye"
printer("ps:")
message = "."
printer1()
