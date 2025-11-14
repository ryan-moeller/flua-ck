local ck = require('ck')

local v = ck.shared.const.new(67)
print(v:load())
local ref = ck.shared.const.retain(v:cookie())
print(ref:load())
print(ref:load())
print(v:load())

local serde_msgpack = require('serde.msgpack')
local const = serde_msgpack.const

local v = const {
	hello = 'world',
	foo = 123,
	yes = true,
	no = false,
	float = 0.2,
}

local ucl = require('ucl')

print(ucl.to_json(v:load()))

local pthread = require('pthread')

local function thread_main(...)
	local ck = require('ck')

	local vars = {}
	for _, cookie in ipairs({...}) do
		table.insert(vars, ck.shared.const.retain(cookie))
	end
	local sum = 0
	for _, var in ipairs(vars) do
		for name, value in pairs(var:load()) do
			print(name, value)
			sum = sum + value
		end
	end
	return sum
end

local vars = {
	const {x=10, y=20, z=67},
	const {q=2, r=4, s=6, t=8},
	const {n=100},
}
local cookies = {}
for _, var in ipairs(vars) do
	table.insert(cookies, var:cookie())
end

local nthreads = 20
local threads = {}
for i = 1, nthreads do
	threads[i] = pthread.create(thread_main, table.unpack(cookies))
end
local sum = 0
for i = 1, nthreads do
	local s = select(2, assert(threads[i]:join()))
	print(i, s)
	sum = sum + s
end
print(sum)
