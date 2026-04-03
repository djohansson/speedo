template <typename LoadOp>
void RHIApplication::InternalOpenFileDialogueAsync(std::string&& resourcePathString, const std::vector<nfdu8filteritem_t>& filterList, LoadOp loadOp)
{
	auto app = std::static_pointer_cast<RHIApplication>(gApplication.lock());
	ENSURE(app);
	auto& rhi = app->GetRHI();
	
	auto [openFileTask, openFileFuture] = CreateTask(
		window::OpenFileDialogue,
		std::move(resourcePathString),
		filterList);

	auto [loadTask, loadFuture] = CreateTask(
		[](auto openFileFuture, auto loadOp)
		{
			ZoneScopedN("RHIApplication::draw::loadTask");

			ENSURE(openFileFuture.Valid());
			ENSURE(openFileFuture.IsReady());

			auto [openFileResult, openFilePath] = openFileFuture.Get();
			if (openFileResult)
			{
				gProgress = 0;
				gShowProgress = true;
				loadOp(openFilePath, gProgress);
				gShowProgress = false;
			}
		},
		std::move(openFileFuture),
		loadOp);

	rhi.mainCalls.enqueue(openFileTask);
	rhi.mainCalls.enqueue(loadTask);
}
