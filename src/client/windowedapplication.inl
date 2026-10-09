namespace client
{

template <typename Dialogue, typename LoadOp>
void WindowedApplication::InternalDialogueAsync(Dialogue dialogue, LoadOp loadOp)
{
	using namespace core;
	
	auto app = std::static_pointer_cast<WindowedApplication>(Application::Get());
	ENSURE(app);

	// only the dialogue needs the main thread. the load is queued to run in the thread pool once the dialogue has
	// returned: on the main thread it would stall window event processing (input, resizes, quitting) while it runs.
	auto [openFileTask, openFileFuture] = CreateTask(
		[dialogue = std::move(dialogue), loadOp = std::move(loadOp)]() mutable
		{
			auto [openFileResult, openFilePath] = dialogue();
			if (!openFileResult)
				return;

			auto name = std::filesystem::path(openFilePath).filename().string();
			(void)gLoads.Enqueue(
				std::move(name),
				[openFilePath = std::move(openFilePath), loadOp = std::move(loadOp)](std::atomic_uint8_t& progress)
				{ return loadOp(openFilePath, progress); });
		}); // captured rather than passed as arguments: CreateTask stores lvalue arguments by reference

	app->mainCalls.enqueue(openFileTask);
}

template <typename LoadOp>
void WindowedApplication::InternalOpenFileDialogueAsync(std::string&& resourcePathString, const std::vector<platform::FileFilter>& filterList, LoadOp loadOp)
{
	InternalDialogueAsync(
		[resourcePathString = std::move(resourcePathString), filterList = std::vector(filterList)]() mutable
		{ return platform::OpenFileDialogue(std::move(resourcePathString), filterList); },
		std::move(loadOp));
}

template <typename LoadOp>
void WindowedApplication::InternalOpenFolderDialogueAsync(std::string&& startPathString, LoadOp loadOp)
{
	InternalDialogueAsync(
		[startPathString = std::move(startPathString)]() mutable { return platform::OpenFolderDialogue(std::move(startPathString)); },
		std::move(loadOp));
}

} // namespace client
