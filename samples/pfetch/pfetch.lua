-- Copyright (c) 2025 Ryan Moeller
--
-- SPDX-License-Identifier: BSD-2-Clause

local bd <const> = require('bsddialog')
local ck <const> = require('ck')
local clock <const> = require('clock')
local posix <const> = require('posix')
local pthread <const> = require('pthread')
local sysctl <const> = require('sysctl')

local ncpu <const> = sysctl('hw.ncpu'):value()

local workq <const> = assert(ck.ring.spmc.new(64))
local workqc <const> = workq:cookie()
local work_nq_ec <const> = assert(ck.ec.ec64.new(0))
local work_nq_ecc <const> = work_nq_ec:cookie()
local work_dq_ec <const> = assert(ck.ec.ec64.new(0))
local work_dq_ecc <const> = work_dq_ec:cookie()

local statusq <const> = assert(ck.ring.mpsc.new(512))
local statusqc <const> = statusq:cookie()
local status_nq_ec <const> = assert(ck.ec.ec64.new(0))
local status_nq_ecc <const> = status_nq_ec:cookie()
local status_dq_ec <const> = assert(ck.ec.ec64.new(0))
local status_dq_ecc <const> = status_dq_ec:cookie()

local function work(id, url, path)
	local format <const> = 'Iss'
	-- Avoid using parameters as upvalues in serde methods.
	-- Stash args in an object instead to access via self.
	return setmetatable({
		id = id,
		url = url,
		path = path,
	}, {
		serialize = function(self, buf)
			buf:write(string.pack(format, self.id, self.url,
			    self.path))
		end,
		deserialize = function(buf)
			local id <const>, url <const>, path <const> =
			    string.unpack(format, buf:read('a'))
			return {
				id = id,
				url = url,
				path = path,
			}
		end,
	})
end

local tasks <const> = {}

function tasks:new(...)
	local task <const> = work(#self + 1, ...)
	if workq:enqueue(task) then
		table.insert(self, task)
		return true
	end
end

-- TODO: spin up a server for testing?
local lfs <const> = require('lfs')
local filesdir <const> = '/storage/ryan/videos'
local files <const> = {}
local x = 0
for ent in lfs.dir(filesdir) do
	if ent ~= '.' and ent ~= '..' then
		local path <const> = table.concat({filesdir, ent}, '/')
		if lfs.attributes(path).mode == 'file' then
			table.insert(files, ('file://%s'):format(path))
		end
	end
end

while tasks:new(files[math.random(#files)], '/dev/null') do
end

local function worker()
	local ck <const> = require('ck')
	local fetch <const> = require('fetch')

	local workq <const> = ck.ring.spmc.retain(workqc)
	local work_nq_ec <const> = ck.ec.ec64.retain(work_nq_ecc)
	local work_dq_ec <const> = ck.ec.ec64.retain(work_dq_ecc)

	local statusq <const> = ck.ring.mpsc.retain(statusqc)
	local status_nq_ec <const> = ck.ec.ec64.retain(status_nq_ecc)
	local status_dq_ec <const> = ck.ec.ec64.retain(status_dq_ecc)

	local function status(id, size, progress, err, code)
		local format <const> = 'ITTsi'
		-- Avoid using parameters as upvalues in serde methods.
		-- Stash args in an object instead to access via self.
		return setmetatable({
			id = id,
			size = size,
			progress = progress,
			err = err,
			code = code,
		}, {
			serialize = function(self, buf)
				buf:write(string.pack(format, self.id,
				    self.size, self.progress, self.err,
				    self.code))
			end,
			deserialize = function(buf)
				local id <const>, size <const>,
				    progress <const>, err <const>,
				    code <const> = string.unpack(format,
				    buf:read('a'))
				return {
					id = id,
					size = size,
					progress = progress,
					err = err,
					code = code,
				}
			end,
		})
	end

	local function finish(id)
		local format <const> = 'I'
		-- Avoid using parameters as upvalues in serde methods.
		-- Stash args in an object instead to access via self.
		return setmetatable({
			id = id,
		}, {
			serialize = function(self, buf)
				buf:write(string.pack(format, self.id))
			end,
			deserialize = function(buf)
				local id <const> = string.unpack(format,
				    buf:read('a'))
				return {
					id = id,
				}
			end,
		})
	end

	local function enqueue(msg)
		repeat
			local ec <const> = status_dq_ec:value()
			if statusq:enqueue(msg) then
				status_nq_ec:inc(ck.ec.mp)
				break
			end
			status_dq_ec:wait(ck.ec.sp, ec)
		until false
	end

	local function report(work, ...)
		enqueue(status(work.id, ...))
	end

	local function done(work)
		enqueue(finish(work.id))
	end

	-- Receive queued work until terminated by a falsey work item.
	repeat
		::continue::
		local ec <const> = work_nq_ec:value()
		local ready <const>, work <const> = workq:dequeue()
		if not ready then
			work_nq_ec:wait(ck.ec.sp, ec)
			goto continue
		end
		work_dq_ec:inc(ck.ec.mp)
		if not work then
			break
		end
		local inf <close>, stat <const>, code <const> =
		    fetch.xget(work.url)
		if not inf then
			local err <const> = stat
			report(work, 0, 0, err, code)
			goto continue
		end
		local size <const> = stat.size
		local fetched = 0
		local progress = 0
		local function report_progress(buflen)
			fetched = fetched + buflen
			local new_progress <const> = (fetched * 100) // size
			if new_progress > progress then
				progress = new_progress
				report(work, size, progress, "", 0)
			end
		end
		local function report_error(err, code)
			report(work, size, progress, err, code)
		end
		report_progress(0)
		local outf <close>, err <const>, code <const> =
		    io.open(work.path, 'w+')
		if not outf then
			report_error(err, code)
			goto continue
		end
		-- fetch.c MINBUFSIZE
		local bufsize <const> = 16384
		inf:setvbuf("full", bufsize)
		-- Save in chunks to report progress periodically.
		repeat
			local buf <const>, err <const>, code <const> =
			    inf:read(bufsize)
			if err then
				report_error(err, code)
				goto continue
			end
			if not buf or #buf == 0 then
				break
			end
			report_progress(#buf)
			local ok <const>, err <const>, code <const> =
			    outf:write(buf)
			if not ok then
				report_error(err, code)
				goto continue
			end
		until false
		done(work)
	until false
end

local workers <const> = {}
for id = 1, ncpu do
	table.insert(workers, assert(pthread.create(worker)))
	-- Each thread has a corresponding cancellation task in the workq.
	repeat
		local ec <const> = work_dq_ec:value()
		if workq:enqueue(nil) then
			work_nq_ec:inc(ck.ec.sp)
			break
		end
		work_dq_ec:wait(ck.ec.mp, ec)
	until false
end

function table.find(t, x)
	for i, y in ipairs(t) do
		if x == y then
			return i
		end
	end
end

bd.init()
local conf <const> = bd.initconf()
conf.title = 'pfetch'
conf.auto_minwidth = 40
conf.text.escape = true
local running <const> = {}
local minibars <const> = {}
assert(bd.mixedgauge(conf, '\nStarting...\n', 0, 0, 0, minibars) == bd.OK)
local finished = 0
local failed = 0
local function runnable()
	return finished + failed < #tasks
end
local last = clock.gettime(clock.REALTIME)
while runnable() do
	::continue::
	local ec <const> = status_nq_ec:value()
	local ready <const>, status <const> = statusq:dequeue()
	if not ready then
		status_nq_ec:wait(ck.ec.mp, ec)
		goto continue
	end
	status_dq_ec:inc(ck.ec.sp)
	assert(status.id)
	local i <const> = table.find(running, status.id)
	if not status.size then
		if i then
			-- TODO: stage removal so status can be shown?
			table.remove(running, i)
			table.remove(minibars, i)
		end
		finished = finished + 1
	elseif status.code == 0 then
		if i then
			-- TODO: nicenum counters?
			minibars[i][2] = status.progress
		else
			local task <const> = tasks[status.id]
			local file <const> = posix.libgen.basename(task.url)
			table.insert(running, task.id)
			table.insert(minibars, {
				("%d: %s"):format(task.id, file),
				bd.MG_INPROGRESS
			})
		end
	else
		if i then
			-- TODO: stage removal so status can be shown?
			table.remove(running, i)
			table.remove(minibars, i)
		end
		failed = failed + 1
	end
	local now <const> = clock.gettime(clock.REALTIME)
	-- Only update once per second unless this is the final pass.
	if (now - last) < 1 and runnable() then
		goto continue
	end
	last = now
	bd.clear(1)
	local text <const> = ('\n%d active, %d/%d finished, %d failed\n')
	    :format(#running, finished, #tasks, failed)
	local active = 0
	for i = 1, #minibars do
		local percent <const> = minibars[i][2]
		if percent > 0 then
			active = active + percent
		end
	end
	local mainperc <const> = (active + (finished + failed) * 100) // #tasks
	assert(bd.mixedgauge(conf, text, 0, 0, mainperc, minibars) == bd.OK)
end
clock.nanosleep(clock.REALTIME, 0, 1) -- one second pause on last screen
bd.clear(1)
local text <const> = ('\n%d/%d finished, %d failed\n')
    :format(finished, #tasks, failed)
conf.clear = true
assert(bd.msgbox(conf, text, 0, 0) == bd.OK)
for _, worker in ipairs(workers) do
	assert(worker:join())
end
bd._end()
