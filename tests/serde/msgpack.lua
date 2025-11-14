local ck = require('ck')

local M = {}
M.__index = M

function M:serialize(s)
	s:write(require('ucl').to_format(self, 'msgpack'))
end

function M.deserialize(s)
	local p = require('ucl').parser()
	p:parse_string(s:read('a*'), 'msgpack')
	return p:get_object()
end

function M.new(t)
	return setmetatable(t, M)
end

function M.const(t)
	return ck.shared.const.new(M.new(t))
end

function M.mut(t)
	return ck.shared.mut.new(M.new(t))
end

return M
