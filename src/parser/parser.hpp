#ifndef PARSER_HPP
#define PARSER_HPP

#include "ast/node.hpp"
#include <deque>
#include <memory>
#include <type_traits>

/**
 * Base class for parsers.
 */
class Parser
{
public:
    /**
     * Destroys a Parser.
     */
    virtual ~Parser() = 0;
    /**
     * Parses the program. The {@link BlockNode} returned is owned by the
     * Parser and will be invalidated once this Parser is destructed.
     *
     * @returns The AST of the program, wrapped in a {@link BlockNode}.
     */
    virtual BlockNode* parse() = 0;

protected:
    /**
     * Creates a Node.
     *
     * @tparam NodeT The Node-derived type to instantiate.
     * @tparam Args NodeT's constructor arguments.
     *
     * @param args NodeT's constructor arguments.
     *
     * @returns A pointer to a newly created NodeT.
     */
    template<typename NodeT, typename... Args>
    typename std::enable_if<std::is_base_of<Node, NodeT>::value, NodeT>::type*
    makeNode(Args&&... args)
    {
        nodes.emplace_back(
            std::make_unique<NodeT>(std::forward<Args>(args)...));
        return static_cast<NodeT*>(nodes.back().get());
    }

private:
    /** Owns all the Nodes that the parser creates. */
    std::deque<std::unique_ptr<Node>> nodes;
};

#endif // PARSER_HPP
