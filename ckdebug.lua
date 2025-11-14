-- Copyright (c) 2025 Ryan Moeller
-- SPDX-License-Identifier: BSD-2-Clause

-- This script was used to debug an infinite loop in ck_hp_reclaim().
-- 
-- $ lldb /usr/libexec/flua
-- (lldb) r test.lua
-- ... flua hangs
-- ^C
-- (lldb) frame select 1
-- (lldb) script dofile 'ckdebug.lua'
--
-- This lldb script expects you to select a stack frame with the record
-- variable in scope.  Then it will check record->pending and
-- record->global->subscribers for cycles.
--
-- In my case it found a cycle in subscribers, caused by erroneously
-- registering recycled records.  With sufficient parallelism, threads were
-- completing and unregistering their records before the last thread was ready
-- to allocate its record, and the recycled record being registered again
-- created a cycle that caused an infinite loop when iterating stack entries.

-- LLDB does this automatically in ScriptInterpreterLua::EnterSession() for
-- interactive REPL sessions.
lldb.debugger = lldb.SBDebugger.FindDebuggerWithID(1)
lldb.target = lldb.debugger:GetSelectedTarget()
lldb.process = lldb.target:GetProcess()
lldb.thread = lldb.process:GetSelectedThread()
lldb.frame = lldb.thread:GetSelectedFrame()

local record = lldb.frame:FindVariable('record')

-- Monkey-patch SBValue and SBType with some convenience methods.
do
	local SBValue = getmetatable(record)
	local SBValue__index = SBValue.__index

	function SBValue:__index(field)
		return lldb.SBValue[field] or SBValue__index(self, field)
	end

	function lldb.SBValue:IsNonNull()
		if self:IsValid() and self:GetType():IsPointerType() then
			local addr = self:GetValueAsAddress()
			assert(addr ~= lldb.LLDB_INVALID_ADDRESS)
			return addr ~= 0
		end
		return false
	end

	assert(record:IsNonNull())

	local SBType = getmetatable(record:GetType())
	local SBType__index = SBType.__index

	function SBType:__index(field)
		return lldb.SBType[field] or SBType__index(self, field)
	end

	function lldb.SBType:GetFieldWithName(name)
		for i = 0, self:GetNumberOfFields() - 1 do
			local field = self:GetFieldAtIndex(i)
			if field:GetName() == name then
				return field
			end
		end
	end
end

local hazard_type = lldb.target:FindFirstType("ck_hp_hazard_t")
local hazard_next_offset = hazard_type:GetFieldWithName("pending_entry"):GetOffsetInBytes()

local function ck_hp_hazard_container(entry)
	assert(entry:IsNonNull())
	local entry_addr = entry:GetValueAsAddress()
	local hazard_addr = entry_addr - hazard_next_offset
	return entry:CreateValueFromAddress(
		"hazard$", hazard_addr, hazard_type
	):AddressOf()
end

function pending_cycle_check(record)
	assert(record:IsNonNull())
	local pending = record:GetChildMemberWithName('pending')
	local visited_entries = {}
	local function visited(entry)
		assert(entry:IsNonNull())
		local key = entry:GetValueAsAddress()
		if visited_entries[key] then
			return true
		end
		visited_entries[key] = true
	end
	local entry = pending:GetChildMemberWithName('head')
	while entry:IsNonNull() and not visited(entry) do
		local hazard = ck_hp_hazard_container(entry)
		assert(hazard:IsNonNull())
		entry = entry:GetChildMemberWithName('next')
	end
	if entry:IsNonNull() then
		local hazard = ck_hp_hazard_container(entry)
		assert(hazard:IsNonNull())
		print('pending cycle', hazard)
		return true
	end
end

pending_cycle_check(record)

local record_type = lldb.target:FindFirstType("ck_hp_record_t")
local record_next_offset = record_type:GetFieldWithName("global_entry"):GetOffsetInBytes()

local function ck_hp_record_container(entry)
	assert(entry:IsNonNull())
	local entry_addr = entry:GetValueAsAddress()
	local record_addr = entry_addr - record_next_offset
	return entry:CreateValueFromAddress(
		"record$", record_addr, record_type
	):AddressOf()
end

function subscribers_cycle_check(record)
	assert(record:IsNonNull())
	local global = record:GetChildMemberWithName('global')
	local subscribers = global:GetChildMemberWithName('subscribers')
	local visited_entries = {}
	local function visited(entry)
		assert(entry:IsNonNull())
		local key = entry:GetValueAsAddress()
		if visited_entries[key] then
			return true
		end
		visited_entries[key] = true
	end
	local function state(record)
		local states = {"CK_HP_USED", "CK_HP_FREE"}
		local state = record:GetChildMemberWithName('state'):GetValueAsUnsigned()
		return ("%s (%d)"):format(states[state + 1], state)
	end
	local function valid(record)
		assert(record:IsNonNull())
		local addr = record:GetValueAsAddress()
		local pointers = record:GetChildMemberWithName('pointers')
		local pointers_addr = pointers:GetValueAsAddress()
		assert(pointers_addr - record_type:GetByteSize() == addr)
		return true
	end
	local entry = subscribers:GetChildMemberWithName('head')
	while entry:IsNonNull() and not visited(entry) do
		local record = ck_hp_record_container(entry)
		assert(valid(record))
		print(record, state(record))
		entry = entry:GetChildMemberWithName('next')
	end
	if entry:IsNonNull() then
		local record = ck_hp_record_container(entry)
		assert(record:IsNonNull())
		print('subscribers cycle', record)
		return true
	end
end

subscribers_cycle_check(record)
