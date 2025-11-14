local ck = require('ck')

local v = ck.shared.mut.new(67)
print(v:load())
local ref = ck.shared.mut.retain(v:cookie())
print(ref:load())
print(ref:load())
print(v:load())
v:store(42)
print(ref:load())

local serde_msgpack = require('serde.msgpack')
local const = serde_msgpack.const
local mut = serde_msgpack.mut

local v = mut {
	hello = 'world',
	foo = 123,
	yes = true,
	no = false,
	float = 0.2,
}

local ucl = require('ucl')

local vlocal = v:load()
print(ucl.to_json(vlocal))
vlocal.float = 1000.0001
vlocal[v:cookie()] = v:cookie()
v:store(vlocal)
print(ucl.to_json(v:load()))

local pthread = require('pthread')

local function thread_main(sumc, ...)
	local ck = require('ck')

	local vars = {}
	for _, cookie in ipairs({...}) do
		table.insert(vars, ck.shared.const.retain(cookie))
	end
	local sum = ck.shared.mut.retain(sumc)
	for _, var in ipairs(vars) do
		for name, value in pairs(var:load()) do
			sum:store(sum:load() + value) -- racy but whatever
		end
	end
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

local sum = ck.shared.mut.new(0)
local sumc = sum:cookie()
local nthreads = 200
local threads = {}
for i = 1, nthreads do
	threads[i] = pthread.create(thread_main, sumc, table.unpack(cookies))
end
for i = 1, nthreads do
	assert(threads[i]:join())
end
print(sum:load())
