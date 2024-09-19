namespace rhiapplication
{

template <typename LoadOp>
void OpenFileDialogueAsync(std::string&& resourcePathString, const std::vector<const nfdu8filteritem_t>& filterList, LoadOp loadOp)
{
	auto app = std::static_pointer_cast<RHIApplication>(Application::Get().lock());
	CHECK(app);
	auto& rhi = app->GetRHI();
	
	auto [openFileTask, openFileFuture] = CreateTask(
		window::OpenFileDialogue,
		std::move(resourcePathString),
		filterList);

	auto [loadTask, loadFuture] = CreateTask(
		[](auto openFileFuture, auto loadOp)
		{
			ZoneScopedN("RHIApplication::draw::loadTask");

			ASSERT(openFileFuture.Valid());
			ASSERT(openFileFuture.IsReady());

			auto [openFileResult, openFilePath] = openFileFuture.Get();
			if (openFileResult)
			{
				RHIApplication::gProgress = 0;
				RHIApplication::gShowProgress = true;
				loadOp(openFilePath, RHIApplication::gProgress);
				RHIApplication::gShowProgress = false;
			}
		},
		std::move(openFileFuture),
		loadOp);

	AddDependency(openFileTask, loadTask);
	rhi.mainCalls.enqueue(openFileTask);
}

} // namespace rhiapplication
