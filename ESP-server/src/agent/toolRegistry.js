function createToolRegistry(tools) {
    const byName = new Map();
    for (const tool of tools) {
        if (!tool?.name || typeof tool.handler !== "function") {
            throw new Error("Each tool requires name and handler");
        }
        if (byName.has(tool.name)) {
            throw new Error(`Duplicate tool: ${tool.name}`);
        }
        byName.set(tool.name, tool);
    }

    function list() {
        return Array.from(byName.values()).map(tool => ({
            name: tool.name,
            description: tool.description,
            parameters: tool.parameters,
            handler: tool.handler
        }));
    }

    function openAiTools() {
        return list().map(tool => ({
            type: "function",
            function: {
                name: tool.name,
                description: tool.description,
                parameters: tool.parameters
            }
        }));
    }

    async function invoke(name, rawArguments, context) {
        const tool = byName.get(name);
        if (!tool) {
            return { success: false, error: `unknown tool: ${name}` };
        }

        let args = {};
        try {
            args = typeof rawArguments === "string" ? JSON.parse(rawArguments || "{}") : (rawArguments || {});
        } catch (_) {
            return { success: false, error: "tool arguments must be valid JSON" };
        }
        if (!args || typeof args !== "object" || Array.isArray(args)) {
            return { success: false, error: "tool arguments must be an object" };
        }

        try {
            return await tool.handler(args, context);
        } catch (error) {
            return { success: false, error: "tool execution failed" };
        }
    }

    return {
        invoke,
        list,
        openAiTools
    };
}

module.exports = {
    createToolRegistry
};
