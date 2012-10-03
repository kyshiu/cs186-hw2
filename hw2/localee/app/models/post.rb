class Post < ActiveRecord::Base
  # attr_accessible :title, :body
  belongs_to :user
  belongs_to :location

  # Validations
  validates :user_id, :presence => true
  validates :location_id, :presence => true

  def to_hash
    {
      :author_id => self.user_id,
      :author => User.find(self.user_id).name,
      :text => self.text,
      :created_at => self.created_at,
      :location => Location.find(self.location_id).to_hash
    }
  end
end
