local M = {}

function M.hello(name)
    return "hello " .. name
end

function M.fail()
    error("deliberate")
end

return M
