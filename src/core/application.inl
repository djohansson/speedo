template <typename T, typename... Args>
std::shared_ptr<T> Application::Create(Args&&... args)
{
	static_assert(std::is_base_of_v<Application, T>);

	// workaround for protected constructors in T
	struct U : public T { constexpr U() = default;
		explicit U(Args&&... args) : T(std::forward<Args>(args)...) {}
	};

	// silly dance to make Application::Get() work during construction
	auto app = std::make_shared_for_overwrite<U>();
	gApplication = app;
	std::construct_at(app.get(), std::forward<Args>(args)...);

	return app;
}
