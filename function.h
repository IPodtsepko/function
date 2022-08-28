#pragma once

#include <type_traits>

struct bad_function_call : std::exception
{
};

namespace function_implementation {

template <typename T>
static constexpr bool
        is_small = (sizeof(T) <= sizeof(void *) &&
                    alignof(T*) % alignof(T) == 0 &&
                    std::is_nothrow_move_constructible<T>::value);

template <typename R, typename... Args>
struct storage_t;

template <typename R, typename... Args>
struct operations_t
{
    using storage_t = function_implementation::storage_t<R, Args...>;

    void (*copy)(storage_t const *, storage_t *);
    void (*move)(storage_t *, storage_t *) noexcept;
       R (*invoke)(storage_t const *, Args...);
    void (*destroy)(storage_t *) noexcept;
};

template <typename R, typename... Args>
struct storage_t
{
    template <typename T>
    T * pointer() const
    {
        return reinterpret_cast<T * const &>(buffer);
    }

    template <typename T>
    T & reference()
    {
        return reinterpret_cast<T &>(buffer);
    }

    template <typename T>
    T const & reference() const
    {
        return reinterpret_cast<T const &>(buffer);
    }

    void set(void * value)
    {
        new (&buffer)(void *)(value);
    }

    operations_t<R, Args...> const * operations;
    std::aligned_storage_t<sizeof(void *), alignof(void *)> buffer;
};

template <typename R, typename... Args>
operations_t<R, Args...> const * get_empty_operations()
{
    using storage_t = function_implementation::storage_t<R, Args...>;

    static constexpr operations_t<R, Args...> operations = {
            /* copy */
            [](storage_t const *, storage_t * destination) {
                destination->operations = get_empty_operations<R, Args...>();
            },
            /* move */
            [](storage_t *, storage_t * destination) noexcept {
                destination->operations = get_empty_operations<R, Args...>();
            },
            /* invoke */
            [](storage_t const *, Args...) -> R {
                throw bad_function_call();
            },
            /* destroy */
            [](storage_t *) noexcept {}};

    return &operations;
}

template <typename T, typename U = void>
struct helper;

template <typename T>
struct helper<T, std::enable_if_t<is_small<T>>>
{
    template <typename Storage>
    static void initialize_storage(Storage & storage, T && value)
    {
        new (&storage.buffer) T(std::move(value));
    }

    template <typename R, typename... Args>
    static operations_t<R, Args...> const * get_operations()
    {
        using storage_t = function_implementation::storage_t<R, Args...>;

        static constexpr operations_t<R, Args...> operations = {
                /* copy */
                [](storage_t const * source, storage_t * destination) {
                    new (&destination->buffer) T(source->template reference<T>());
                    destination->operations = source->operations;
                },
                /* move */
                [](storage_t * source, storage_t * destination) noexcept {
                    new (&destination->buffer) T(std::move(source->template reference<T>()));
                    destination->operations = source->operations;
                },
                /* invoke */
                [](storage_t const * storage, Args... args) -> R {
                    return (storage->template reference<T>())(std::forward<Args>(args)...);
                },
                /* destroy */
                [](storage_t * storage) noexcept {
                    storage->template pointer<T>()->~T();
                }};

        return &operations;
    }

    template <typename Storage>
    static T * target(Storage & storage)
    {
        return &reinterpret_cast<T &>(storage.buffer);
    }

    template <typename Storage>
    static T const * target(Storage const & storage)
    {
        return &reinterpret_cast<T const &>(storage.buffer);
    }
};

template <typename T>
struct helper<T, std::enable_if_t<!is_small<T>>>
{
    template <typename Storage>
    static void initialize_storage(Storage & storage, T && value)
    {
        storage.set(new T(std::move(value)));
    }

    template <typename R, typename... Args>
    static operations_t<R, Args...> const * get_operations()
    {

        using storage_t = function_implementation::storage_t<R, Args...>;

        static constexpr operations_t<R, Args...> operations = {
                /* copy */
                [](storage_t const * source, storage_t * destination) {
                    destination->set(new T(*source->template pointer<T>()));
                    destination->operations = source->operations;

                },
                /* move */
                [](storage_t * source, storage_t * destination) noexcept {
                    destination->operations = source->operations;
                    destination->set((void *)source->template pointer<T>());
                    source->operations = get_empty_operations<R, Args...>();
                },
                /* invoke */
                [](storage_t const * storage, Args... args) -> R {
                    return (*storage->template pointer<T>())(std::forward<Args>(args)...);
                },
                /* destroy */
                [](storage_t * storage) noexcept {
                    delete storage->template pointer<T>();
                }};

        return &operations;
    }

    template <typename Storage>
    static T * target(Storage & storage)
    {
        return storage.template pointer<T>();
    }

    template <typename Storage>
    static T const * target(Storage const & storage)
    {
        return storage.template pointer<T>();
    };
};
} // namespace function_implementation

template <typename T>
struct function;

template <typename R, typename... Args>
struct function<R(Args...)>
{
    template <typename T>
    using helper = function_implementation::helper<T>;

    using storage_t = function_implementation::storage_t<R, Args...>;

    function()
    {
        m_storage.operations = function_implementation::get_empty_operations<R, Args...>();
    }

    template <typename T>
    function(T value)
    {
        helper<T>::initialize_storage(m_storage, std::move(value));
        m_storage.operations = helper<T>::template get_operations<R, Args...>();
    }

    function(function const & other)
    {
        other.m_storage.operations->copy(&other.m_storage, &m_storage);
    }

    function(function && other) noexcept
    {
        other.m_storage.operations->move(&other.m_storage, &m_storage);
    }


    function & operator=(function const & other)
    {
        if (this != &other) {
            storage_t buffer;

            other.m_storage.operations->copy(&other.m_storage, &buffer);

            m_storage.operations->destroy(&m_storage);

            buffer.operations->move(&buffer, &m_storage);
            buffer.operations->destroy(&buffer);
        }
        return *this;
    }

    function & operator=(function && other) noexcept
    {
        if (this != &other) {
            m_storage.operations->destroy(&m_storage);
            other.m_storage.operations->move(&other.m_storage, &m_storage);
        }
        return *this;
    }

    R operator()(Args... args) const
    {
        return m_storage.operations->invoke(&m_storage, std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept
    {
        return m_storage.operations != function_implementation::get_empty_operations<R, Args...>();
    }

    template <typename T>
    T * target() noexcept
    {
        if (m_storage.operations == helper<T>::template get_operations<R, Args...>()) {
            return helper<T>::target(m_storage);
        }
        return nullptr;
    }

    template <typename T>
    T const * target() const noexcept
    {
        if (m_storage.operations == helper<T>::template get_operations<R, Args...>()) {
            return helper<T>::target(m_storage);
        }
        return nullptr;
    }

    ~function()
    {
        m_storage.operations->destroy(&m_storage);
    }

    storage_t m_storage;
};