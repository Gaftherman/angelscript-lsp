#include <iostream>
#include "lsp/Server.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main()
{
    try
    {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif

        angel_lsp::Server server;
        server.Run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

class Vector3;

/**
 * @class ThreadSafeQueue
 * @brief A first-in, first-out (FIFO) queue with blocking pop operations.
 *
 * @details The class wraps a standard std::queue container. It enforces thread
 * safety by guarding all internal modifications with an internal mutex.
 *
 * ### Usage Example
 * @code{.cpp}
 * concurrent::ThreadSafeQueue<int> queue;
 * queue.push(42);
 *
 * int value;
 * if (queue.try_pop(value)) {
 *     // Process value
 * }
 * @endcode
 *
 * @tparam T The type of the elements stored in the queue. Must be move-constructible.
 */
struct IEntity
{

    /**
     * @brief Spawns an entity in the game world.
     * @details This method is responsible for creating and placing an entity in the game world at the specified position.
     *
     * @param[in] pos The position where the entity should be spawned.
     */
    void Spawn(Vector3 pos);
    /**
     * @brief Removes an entity from the game world.
     * @details This method is responsible for removing or deactivating an entity from the game world, ensuring that all associated resources are properly released and that the entity is no longer active in the simulation.
     */
    void Despawn();

    /**
     * @brief Pushes a new element into the back of the queue.
     * @details Takes ownership of the provided element via rvalue reference,
     * safely locks the internal data array, and signals exactly one waiting thread.
     *
     * @param[in] new_value The item to add. The element is moved into the container.
     * @return true If an element was successfully popped.
     * @return false If the queue was empty at the exact millisecond of evaluation.
     *
     * @note This operation is non-blocking and provides a strong exception guarantee.
     */
    void Update(float deltaTime);

    float member; // Comment of the member
};
