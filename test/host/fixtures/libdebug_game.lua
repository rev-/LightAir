-- Fixture: a runtime error in the GAME file, with the libraries loaded.
--
-- Libraries are stripped of debug information; game files never are —
-- they are what players edit, and "file.lua:LINE: ... (local 'x')" on the
-- failure screen is the only debugger they have.  The error below is on
-- line 9 and names the local.
local proj = la.lib("projector")
local t = nil
return t.x
