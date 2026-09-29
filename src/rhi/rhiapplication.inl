namespace rhi
{

template <typename LoadOp>
auto RHIApplication::InternalOpenFileDialogueAsync(std::string&& resourcePathString, const std::vector<nfdu8filteritem_t>& filterList, LoadOp loadOp)
{
	using namespace core;
	
	auto app = std::static_pointer_cast<RHIApplication>(Application::Get());
	ENSURE(app);
	auto& rhi = app->GetRHI();
	
	auto [openFileTask, openFileFuture] = CreateTask(
		window::OpenFileDialogue,
		std::move(resourcePathString),
		std::vector(filterList)); // by value, see loadOp below

	auto [loadTask, loadFuture] = CreateTask(
		[](auto openFileFuture, auto loadOp) -> std::invoke_result_t<LoadOp, const std::string&, std::atomic_uint8_t&>
		{
			ZoneScopedN("RHIApplication::draw::loadTask");

			ENSURE(openFileFuture.Valid());
			ENSURE(openFileFuture.IsReady());

			auto [openFileResult, openFilePath] = openFileFuture.Get();
			if (openFileResult)
			{
				gProgress = 0;
				gShowProgress = true;
				auto result = loadOp(openFilePath, gProgress);
				gShowProgress = false;
				return result;
			}
			return {};
		},
		std::move(openFileFuture),
		std::move(loadOp)); // by value: CreateTask stores lvalue arguments by reference, and loadOp dies when we return

	rhi.mainCalls.enqueue(openFileTask);
	rhi.mainCalls.enqueue(loadTask);

	return loadFuture;
}

} // namespace rhi
