SELECT count(*)
FROM Message_hasTag_Tag_T, Comment_replyOf_Message_T, Comment_hasTag_Tag AS cht1, Comment_hasTag_Tag AS cht2
WHERE Message_hasTag_Tag_T.MessageId = Comment_replyOf_Message_T.ParentMessageId
	AND Message_hasTag_Tag_T.TagId = cht1.TagId
  AND Comment_replyOf_Message_T.CommentId = cht2.CommentId
  AND Message_hasTag_Tag_T.TagId < cht2.TagId