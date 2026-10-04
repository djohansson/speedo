namespace rhi
{

template <typename LoadOp>
void RHIApplication::InternalOpenFileDialogueAsync(std::string&& resourcePathString, const std::vector<window::FileFilter>& filterList, LoadOp loadOp)
{
	using namespace core;
	
	auto app = std::static_pointer_cast<RHIApplication>(Application::Get());
	ENSURE(app);
	auto& rhi = app->GetRHI();

	// only the dialogue needs the main thread. the load is queued to run in the thread pool once the dialogue has
	// returned: on the main thread it would stall window event processing (input, resizes, quitting) while it runs.
	auto [openFileTask, openFileFuture] = CreateTask(
		[resourcePathString = std::move(resourcePathString), filterList = std::vector(filterList), loadOp = std::move(loadOp)]() mutable
		{
			auto [openFileResult, openFilePath] = window::OpenFileDialogue(std::move(resourcePathString), filterList);
			if (!openFileResult)
				return;

			auto name = std::filesystem::path(openFilePath).filename().string();
			(void)gLoads.Enqueue(
				std::move(name),
				[openFilePath = std::move(openFilePath), loadOp = std::move(loadOp)](std::atomic_uint8_t& progress)
				{ return loadOp(openFilePath, progress); });
		}); // captured rather than passed as arguments: CreateTask stores lvalue arguments by reference

	rhi.mainCalls.enqueue(openFileTask);
}

} // namespace rhi
