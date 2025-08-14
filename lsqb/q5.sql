SELECT count(*)
FROM Message_hasTag_Tag_T, Comment_replyOf_Message_T, Comment_hasTag_Tag AS cht
WHERE Message_hasTag_Tag_T.MessageId = Comment_replyOf_Message_T.ParentMessageId
	AND Comment_replyOf_Message_T.CommentId = cht.CommentId
	AND Message_hasTag_Tag_T.TagId < cht.TagId